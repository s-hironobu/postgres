/*-------------------------------------------------------------------------
 *
 * pg_undo_dump.c - decode and display UNDO log
 *
 * Copyright (c) 2013-2020, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  src/bin/pg_undo_dump/pg_undo_dump.c
 *-------------------------------------------------------------------------
 */

#define FRONTEND 1
#include "postgres.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "access/undolog.h"
#include "access/undopage.h"
#include "access/undorecordset.h"
#include "common/logging.h"
#include "common/fe_memutils.h"

/*
 * #include "access/undorecordset_xlog.h"
 * #include "catalog/database_internal.h"
 */
#include "getopt_long.h"

static const char *progname;

typedef struct UndoSegFile
{
	char	*name;
	UndoLogNumber	logno;
	UndoLogOffset offset;
} UndoSegFile;

/*
 * Open the file in the valid target directory.
 *
 * return a read only fd
 */
static int
open_file_in_directory(const char *directory, UndoSegFile *seg)
{
	int			fd = -1;
	char		fpath[MAXPGPATH];

	Assert(directory != NULL);

	snprintf(fpath, MAXPGPATH, "%s/%06X.%010zX",
			directory, seg->logno, seg->offset);

	fd = open(fpath, O_RDONLY | PG_BINARY, 0);

	if (fd < 0)
		pg_log_error("could not open file \"%s\": %s",
					 seg->name, strerror(errno));
	return fd;
}

static void
usage(void)
{
	printf(_("%s decodes and displays PostgreSQL undo logs for debugging.\n\n"),
		   progname);
	printf(_("Usage:\n  %s [OPTION]... DATADIR\n\n"), progname);
	printf(_("\nOptions:\n"));
	printf(_(" [-D, --pgdata=]DATADIR          data directory\n"));
	printf(_("  -?, --help             show this help, then exit\n"));
	printf(_("\nReport bugs to <pgsql-bugs@lists.postgresql.org>.\n"));
}

/*
 * Each log consists of multiple segments, which need to be processed by the
 * offset. To ensure that the logs are sorted separately, sort first by logno,
 * then by offset.
 */
static int
undo_seg_compare(const void *s1, const void *s2)
{
	UndoSegFile	*seg1 = (UndoSegFile *) s1;
	UndoSegFile	*seg2 = (UndoSegFile *) s2;

	if (seg1->logno != seg2->logno)
		return seg1->logno > seg2->logno ? 1 : -1;

	if (seg1->offset != seg2->offset)
		return seg1->offset > seg2->offset ? 1 : -1;

	return 0;
}

static void
print_chunk_info(UndoRecPtr start, UndoRecPtr prev, UndoLogOffset size)
{
	UndoLogNumber logno = UndoRecPtrGetLogNo(start);
	UndoLogOffset off = UndoRecPtrGetOffset(start);
	UndoLogNumber logno_prev = UndoRecPtrGetLogNo(prev);
	UndoLogOffset off_prev = UndoRecPtrGetOffset(prev);

	printf("logno: %d, start: %010zX, prev: %X.%010zX, size: %zu\n",
		   logno, off, logno_prev, off_prev, size);
}

/*
 * Process segments of a single log file. Return prematurely if any error is
 * encountered.
 *
 * prev_chunk is in/out argument that helps to maintain the pointer to the
 * previous chunk across calls.
 */
static void
process_log(const char *dir_path, UndoSegFile *first, int count,
			UndoRecPtr *prev_chunk)
{
	int	i;
	UndoSegFile	*seg = first;
	UndoLogOffset	off_expected = 0;
	char	buf[UndoLogSegmentSize];
	UndoRecPtr	current_chunk = InvalidUndoRecPtr;
	UndoRecordSetChunkHeader	chunk_hdr;
	int	chunk_hdr_bytes_left = 0;
	UndoLogOffset	chunk_bytes_left = 0;

	/* This is very unlikely, but easy to check. */
	if (count > (UndoLogMaxSize / UndoLogSegmentSize))
	{
		pg_log_error("log %d has too many segments", first->logno);
		return;
	}

	for (i = 0; i < count; i++)
	{
		int	seg_file;
		int	nread, j;
		char	*p;

		/*
		 * Since the UNDO log is a continuous stream of changes, any hole
		 * terminates processing.
		 */
		if (seg->offset != off_expected)
		{
			pg_log_error("segment %010zX missing in log %d", seg->offset,
						 seg->logno);
			return;
		}

		/* Open the segment file and read it. */
		seg_file = open_file_in_directory(dir_path, seg);
		if (seg_file < 0)
			return;

		nread = read(seg_file, buf, UndoLogSegmentSize);
		if (nread != UndoLogSegmentSize)
		{
			if (nread <= 0)
				pg_log_error("could not read from log file %s: %s",
							 seg->name, strerror(errno));
			else
				pg_log_error("could only read %d bytes out of %zu from log file %s: %s",
							 nread, UndoLogSegmentSize, seg->name,
							 strerror(errno));
			return;
		}

		/* Process the pages of the segment. */
		p = buf;
		for (j = 0; j < UndoLogSegmentSize / BLCKSZ; j++)
		{
			UndoPageHeaderData	pghdr;
			int	page_offset = 0;
			int	page_usage;
			uint16	first_rec;
			uint16	first_chunk = 0;

			/* The segment is not loaded aligned. */
			memcpy(&pghdr, p, SizeOfUndoPageHeaderData);

			page_usage = pghdr.ud_insertion_point;

			/*
			 * TODO Consider continuing the scan but only checking that all
			 * the remaining pages (including headers?) are uninitialized.
			 */
			if (page_usage == 0)
				return;

			if (page_usage < SizeOfUndoPageHeaderData || page_usage > BLCKSZ)
			{
				pg_log_error("page %d of the log segment \"%s\" has invalid ud_insertion_point: %d",
							 j, seg->name, page_usage);
				return;
			}

			first_rec = pghdr.ud_first_record;
			if (first_rec != 0 &&
				(first_rec < SizeOfUndoPageHeaderData||
				 first_rec >= pghdr.ud_insertion_point))
			{
				pg_log_error("page %d of the log segment \"%s\" has invalid ud_first_record: %d",
							 j, seg->name, pghdr.ud_first_record);
				return;
			}

			/* The log should start with a chunk. */
			if (i == 0 && j == 0)
			{
				/* Check as most as we can of the page header. */
				if (pghdr.ud_first_chunk != SizeOfUndoPageHeaderData)
				{
					pg_log_error("the initial segment (\"%s\") does not start with a chunk immediately following the page header",
								 seg->name);
					return;
				}
				first_chunk = pghdr.ud_first_chunk;

				if (pghdr.ud_continue_chunk != InvalidUndoRecPtr)
				{
					pg_log_error("chunk continues on the initial segment \"%s\"",
								 seg->name);
					return;
				}

				current_chunk = MakeUndoRecPtr(seg->logno, seg->offset +
											   pghdr.ud_first_chunk);
				chunk_hdr_bytes_left = SizeOfUndoRecordSetChunkHeader;
			}

			page_offset = SizeOfUndoPageHeaderData;

			/*
			 * If we are currently processing chunk data, the page header must
			 * be subtracted because it's included in the chunk size.
			 */
			if (chunk_bytes_left > 0)
			{
				if (chunk_bytes_left < SizeOfUndoPageHeaderData)
				{
					UndoLogNumber logno = UndoRecPtrGetLogNo(current_chunk);
					UndoLogOffset offset = UndoRecPtrGetOffset(current_chunk);

					pg_log_error("chunk starting at %06X.%010zX has invalid size %zu",
								 logno, offset, chunk_hdr.size);
					return;
				}

				chunk_bytes_left -= SizeOfUndoPageHeaderData;
			}

			/* Process the page data. */
			while (page_offset < page_usage)
			{
				/*
				 * At any moment we're reading either the chunk or chunk
				 * header, but never both.
				 */
				Assert(!(chunk_hdr_bytes_left > 0 && chunk_bytes_left > 0));

				if (chunk_hdr_bytes_left > 0)
				{
					int	done, read_now;

					/*
					 * Retrieve the remaining part of the header that fits on
					 * the current page.`
					 */
					done = SizeOfUndoRecordSetChunkHeader - chunk_hdr_bytes_left;
					read_now = Min(chunk_hdr_bytes_left, page_usage - page_offset);
					memcpy((char *) &chunk_hdr + done, p + page_offset, read_now);
					chunk_hdr_bytes_left -= read_now;
					page_offset  += read_now;

					/*
					 * If we have got the whole header, get the chunk size,
					 * otherwise continue reading the header.
					 */
					if (chunk_hdr_bytes_left == 0)
					{
						if (chunk_hdr.size < SizeOfUndoRecordSetChunkHeader ||
							/* TODO Check the "usable bytes" instead. */
							chunk_hdr.size > UndoLogMaxSize)
						{
							UndoLogNumber logno = UndoRecPtrGetLogNo(current_chunk);
							UndoLogOffset offset = UndoRecPtrGetOffset(current_chunk);

							pg_log_error("chunk starting at %06X.%010zX has invalid size %zu",
										 logno, offset, chunk_hdr.size);
							return;
						}

						print_chunk_info(current_chunk, chunk_hdr.previous_chunk,
										 chunk_hdr.size);

						/*
						 * *prev_chunk is not aware of the fact that the first
						 * chunk header in the URS has previous_chunk invalid,
						 * so only check valid previous_chunk.
						 */
						if (chunk_hdr.previous_chunk != *prev_chunk &&
							chunk_hdr.previous_chunk != InvalidUndoRecPtr)
						{
							UndoLogNumber logno = UndoRecPtrGetLogNo(current_chunk);
							UndoLogOffset offset = UndoRecPtrGetOffset(current_chunk);

							pg_log_error("chunk starting at %06X.%010zX has invalid previous_chunk link",
										 logno, offset);
						}

						/* The header is included in the chunk size. */
						Assert(chunk_hdr.size >= SizeOfUndoRecordSetChunkHeader);
						chunk_bytes_left = chunk_hdr.size - SizeOfUndoRecordSetChunkHeader;
					}
					else
						continue;
				}

				/* Process the current chunk. */
				if (chunk_bytes_left > 0)
				{
					int	read_now;

					read_now = Min(chunk_bytes_left, page_usage - page_offset);
					chunk_bytes_left -= read_now;
					page_offset += read_now;
				}

				/*
				 * If done with the current chunk, prepare to read the next
				 * one.
				 */
				if (chunk_bytes_left == 0)
				{
					chunk_hdr_bytes_left = SizeOfUndoRecordSetChunkHeader;
					*prev_chunk = current_chunk;
					/* The following chunk is becoming the current one. */
					current_chunk = MakeUndoRecPtr(seg->logno, seg->offset +
												   j * BLCKSZ + page_offset);

					/*
					 * Save the offset of the first chunk start, to check the
					 * value stored in the header.
					 */
					if (first_chunk == 0)
						first_chunk = page_offset;
				}
			}

			/* Check ud_first_chunk. */
			if (pghdr.ud_first_chunk != first_chunk)
			{
				/*
				 * current_chunk is where the next chunk should start, but
				 * that chunk might not exist yet. In such a case,
				 * ud_first_chunk is still zero and should not be checked.
				 */
				if ((current_chunk % BLCKSZ) != page_offset)
				{
					pg_log_error("page %d of the log segment \"%s\" has invalid ud_first_chunk: %d",
								 j, seg->name, pghdr.ud_first_chunk);
					return;
				}
#ifdef USE_ASSERT_CHECKING
				else
					Assert(pghdr.ud_first_chunk == 0);
#endif	/* USE_ASSERT_CHECKING */
			}

			p += BLCKSZ;
		}


		close(seg_file);
		off_expected += UndoLogSegmentSize;
		seg++;
	}
}

static void
process_directory(const char *dir_path)
{
	DIR		   *dir;
	struct dirent *de;
	UndoSegFile	*segments, *seg, *log_start;
	int	nseg, nseg_max, i;
	UndoRecPtr	prev_chunk = InvalidUndoRecPtr;

	dir = opendir(dir_path);
	if (dir == NULL)
	{
		pg_log_error("could not open directory \"%s\": %m", dir_path);
		exit(1);
	}

	nseg_max = 8;
	nseg = 0;
	segments = (UndoSegFile *) palloc(nseg_max * sizeof(UndoSegFile));

	/* First, collect information on all segments. */
	while (errno = 0, (de = readdir(dir)) != NULL)
	{
		UndoLogNumber logno;
		UndoLogOffset offset;
		int offset_high;
		int offset_low;

		if ((strcmp(de->d_name, ".") == 0) ||
			(strcmp(de->d_name, "..") == 0))
			continue;

		if (strlen(de->d_name) != 17 ||
			sscanf(de->d_name, "%06X.%02X%08X",
				   &logno, &offset_high, &offset_low) != 3)
		{
			pg_log_info("unexpected file \"%s\" in \"%s\"", de->d_name, dir_path);
			continue;
		}

		offset = ((UndoLogOffset) offset_high << 32) | offset_low;

		if (nseg >= nseg_max)
		{
			nseg_max *= 2;
			segments = (UndoSegFile *) repalloc(segments,
												nseg_max * sizeof(UndoSegFile));
		}
		seg = &segments[nseg++];
		seg->name = pg_strdup(de->d_name);
		seg->logno = logno;
		seg->offset = offset;
	}

	if (errno)
	{
		pg_log_error("could not read directory \"%s\": %m", dir_path);
		goto cleanup;
	}

	if (closedir(dir))
	{
		pg_log_error("could not close directory \"%s\": %m", dir_path);
		goto cleanup;
	}

	/*
	 * The segments need to be processed in the offset order, so sort them.
	 */
	qsort((void *) segments, nseg, sizeof(UndoSegFile), undo_seg_compare);

	/* Process the per-log sequences. */
	seg = log_start = segments;
	for (i = 0; i < nseg; i++)
	{
		/* Reached the end or a new log? */
		if (i == nseg || seg->logno != log_start->logno)
		{
			process_log(dir_path, log_start, seg - log_start, &prev_chunk);
			log_start = seg;
		}

		seg++;
	}
	if (seg > log_start)
		process_log(dir_path, log_start, seg - log_start, &prev_chunk);

cleanup:
	for (i = 0; i < nseg; i++)
	{
		seg = &segments[i];
		pfree(seg->name);
	}
	pfree(segments);
}

int
main(int argc, char **argv)
{
	char	   *DataDir = NULL;

	static struct option long_options[] = {
		{"pgdata", required_argument, NULL, 'D'},
		{NULL, 0, NULL, 0}
	};

	int			option;
	int			optindex = 0;

	pg_logging_init(argv[0]);
	set_pglocale_pgservice(argv[0], PG_TEXTDOMAIN("pg_waldump"));
	progname = get_progname(argv[0]);

	if (argc > 1)
	{
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
		{
			usage();
			exit(0);
		}
		if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)
		{
			puts("pg_undo_dump (PostgreSQL) " PG_VERSION);
			exit(0);
		}
	}

	while ((option = getopt_long(argc, argv, "D:",
								 long_options, &optindex)) != -1)
	{
		switch (option)
		{
			case 'D':
				DataDir = optarg;
				break;

			default:
				goto bad_argument;
		}
	}

	if ((optind + 1) < argc)
	{
		pg_log_error("too many command-line arguments (first is \"%s\")",
					 argv[optind + 1]);
		goto bad_argument;
	}

	if (DataDir == NULL && optind < argc)
		DataDir = argv[optind++];

	if (DataDir == NULL)
	{
		pg_log_error("no data directory specified");
		fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname);
		exit(1);
	}

	if (chdir(DataDir) < 0)
	{
		pg_log_error("could not change directory to \"%s\": %m",
					 DataDir);
		exit(1);
	}

	/* TODO Process tablespaces too. */
	process_directory("base/undo");

	return EXIT_SUCCESS;

bad_argument:
	fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname);
	return EXIT_FAILURE;
}

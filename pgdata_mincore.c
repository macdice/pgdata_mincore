#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define OS_BLOCK_SIZE 4096
#define PG_BLOCK_SIZE 8192

static int
usage(const char *executable)
{
	fprintf(stderr, "Usage: %s pgdata, or %s pgdata dbnode relfilenode\n",
			executable, executable);
	return 1;
}

static int
dump_one_relation(const char *pgdata, uint32_t db, uint32_t filenode)
{
	char pages[1024];
	char path[1024];
	int	segno;
	int fd;
	uint32_t block = 0;

	for (segno = 0; ; ++segno)
	{
		struct stat stat_buf;
		char *base;
		char *p;

		if (segno == 0)
			snprintf(path, sizeof(path), "%s/base/%u/%u", pgdata, db, filenode);
		else
			snprintf(path, sizeof(path), "%s/base/%u/%u.%u", pgdata, db, filenode, segno);

		if (lstat(path, &stat_buf) < 0)
		{
			if (errno == ENOENT)
				break;
			perror("lstat");
			return 1;
		}

		if (stat_buf.st_size == 0)
			continue;

		fd = open(path, O_RDONLY);
		if (fd < 0)
		{
			perror("open");
			return 1;
		}

		base = mmap(NULL, stat_buf.st_size, PROT_READ, MAP_FILE | MAP_PRIVATE, fd, 0);
		if (base == MAP_FAILED)
		{
			perror("mmap");
			close(fd);
			return 1;
		}

		p = base;
		while (p < (base + stat_buf.st_size))
		{
			size_t i;

			if (mincore(p, sizeof(pages) * OS_BLOCK_SIZE, pages) < 0)
			{
				if (errno != ENOMEM)
				{
					perror("mincore");
					munmap(base, stat_buf.st_size);
					close(fd);
					return 1;
				}
				goto skip;
			}
			/* we are assuming 4096 byte OS pages, 8192 byte PG pages */
			for (i = 0; i < sizeof(pages); i += 2)
			{
				int count = 0;

				if (pages[i] & MINCORE_INCORE)
					++count;
				if (pages[i + 1] & MINCORE_INCORE)
					++count;

				if (count > 0)
					printf("%u\t%u\t%u\t%d\n", db, filenode, block, count);

				++block;
			}
skip:
			p += sizeof(pages) * OS_BLOCK_SIZE;
		}

		munmap(base, stat_buf.st_size);
		close(fd);
	}
	return 0;
}

static int
dump_all(const char *pgdata)
{
	char base_path[1024];
	DIR *base_dir;
	DIR *db_dir;
	struct dirent *base_dir_ent;
	struct dirent *db_dir_ent;

	snprintf(base_path, sizeof(base_path), "%s/base", pgdata);
	base_dir = opendir(base_path);
	if (base_dir == NULL)
	{
		perror("opendir");
		return 1;
	}
	while ((base_dir_ent = readdir(base_dir)))
	{
		char db_path[1024];
		uint32_t db;

		if (strcmp(base_dir_ent->d_name, ".") == 0 ||
			strcmp(base_dir_ent->d_name, "..") == 0)
			continue;
		db = strtol(base_dir_ent->d_name, NULL, 10);
		snprintf(db_path, sizeof(db_path), "%s/base/%s", pgdata,
				 base_dir_ent->d_name);
		db_dir = opendir(db_path);
		if (db_dir  == NULL)
		{
			perror("opendir");
			closedir(base_dir);
			return 1;
		}
		while ((db_dir_ent = readdir(db_dir)))
		{
			uint32_t filenode;

			if (strcmp(db_dir_ent->d_name, ".") == 0 ||
				strcmp(db_dir_ent->d_name, "..") == 0)
				continue;
			if (strchr(db_dir_ent->d_name, '.') != NULL ||
				strchr(db_dir_ent->d_name, '_') != NULL)
				continue;
			filenode = strtol(db_dir_ent->d_name, NULL, 10);

			dump_one_relation(pgdata, db, filenode);
		}
		closedir(db_dir);
	}
	closedir(base_dir);

	return 0;
}

int
main(int argc, char **argv)
{
	if (argc == 2)
		return dump_all(argv[1]);
	else if (argc == 4)
		return dump_one_relation(argv[1],
								 strtoll(argv[2], NULL, 10),
								 strtoll(argv[3], NULL, 10));
	else
		return usage(argv[0]);
}

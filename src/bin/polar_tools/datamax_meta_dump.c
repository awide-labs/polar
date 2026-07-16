#define FRONTEND 1
#include "postgres.h"

#include "common/file_perm.h"
#include "polar_vfs/polar_vfs_fe.h"
#include "polar_tools.h"
#include "polar_datamax/polar_datamax_internal.h"
#include "utils/pg_crc.h"

static struct option long_options[] =
{
	{"file_path", required_argument, NULL, 'f'},
	{"storage_mode", required_argument, NULL, 's'},
	{"polar_disk_name", required_argument, NULL, 'n'},
	{"polar_storage_cluster_name", required_argument, NULL, 'c'},
	{"polar_hostid", required_argument, NULL, 'i'},
	{"help", no_argument, NULL, '?'},
	{NULL, 0, NULL, 0}
};

static void
usage(void)
{
	printf("Dump datamax meta usage:\n");
	printf("-f, --file_path  					Specify datamax meta file path\n");
	printf("-s, --storage_mode 					Specify meta file storage mode, l for local_mode, p for pfs_mode, default l\n");
	printf("-n, --polar_disk_name=disk			Specify datamax polar_disk_name for pfs mode\n");
	printf("-c, --polar_storage_cluster_name=cluster_name  Specify polar_storage_cluster_name for pfs mode\n");
	printf("-i, --polar_hostid=host_id, 		Specify polar_host_id for pfs mode\n");
	printf("-?, --help show this help, then exit\n");
}

static pg_crc32c
polar_calc_meta_crc(unsigned char *data, size_t len)
{
	pg_crc32c	crc;

	INIT_CRC32C(crc);
	COMP_CRC32C(crc, data, len);
	FIN_CRC32C(crc);

	return crc;
}

static int
polar_dump_datamax_meta(char *file_path)
{
	polar_datamax_meta_data_t meta;
	pg_crc32c	crc;
	struct stat stat_buf;
	int			fd = -1;
	int			ret = -1;

	if (file_path == NULL)
	{
		fprintf(stderr, "file path is NULL, need to be specified validly\n");
		goto end;
	}
	if (polar_stat(file_path, &stat_buf) != 0)
	{
		fprintf(stderr, _("file \"%s\" doesn't exist\n"), file_path);
		goto end;
	}

	fd = polar_open(file_path, O_RDONLY | PG_BINARY, pg_file_create_mode);
	if (fd < 0)
	{
		fprintf(stderr, _("could not open file \"%s\": %s\n"), file_path, strerror(errno));
		goto end;
	}
	if (polar_read(fd, &meta, sizeof(polar_datamax_meta_data_t)) != sizeof(polar_datamax_meta_data_t))
	{
		fprintf(stderr, _("could not read file \"%s\": %s\n"), file_path, strerror(errno));
		goto end;
	}

	/* check value */
	crc = meta.crc;

	if (meta.magic != POLAR_DATAMAX_MAGIC)
		fprintf(stderr, _("The magic number of meta file is incorrect, got %d, expect %d"),
				meta.magic, POLAR_DATAMAX_MAGIC);

	if (meta.version != POLAR_DATAMAX_VERSION)
		fprintf(stderr, _("The version is incorrect, got %d, expect %d"), meta.version, POLAR_DATAMAX_VERSION);

	meta.crc = 0;
	meta.crc = polar_calc_meta_crc((unsigned char *) &meta, sizeof(polar_datamax_meta_data_t));

	if (crc != meta.crc)
		fprintf(stderr, "The crc is incorrect, got %u, expect %u", crc, meta.crc);

	fprintf(stdout, "magic=%x version=%x min_timeline_id=%u min_received_lsn=%lx last_timeline_id=%u last_received_lsn=%lx last_valid_received_lsn=%lx upstream_last_removed_segno=%lx crc=%x\n",
			meta.magic, meta.version, meta.min_timeline_id, meta.min_received_lsn,
			meta.last_timeline_id, meta.last_received_lsn, meta.last_valid_received_lsn, meta.upstream_last_removed_segno, crc);
	ret = 0;

end:
	if (fd >= 0)
		polar_close(fd);
	return ret;
}

int
datamax_meta_main(int argc, char **argv)
{
	int			option;
	int			optindex = 0;
	char	   *file_path = NULL;
	char	   *polar_storage_cluster_name = NULL;
	char	   *polar_disk_name = NULL;
	int			polar_hostid = 0;
	bool		pfs_mode = false,
				init_fs = false;
	bool		succeed = false;

	if (argc <= 2)
	{
		usage();
		return -1;
	}

	while ((option = getopt_long(argc, argv, "f:s:n:c:i:?", long_options, &optindex)) != -1)
	{
		switch (option)
		{
			case 'f':
				file_path = pg_strdup(optarg);
				break;
			case 's':
				if (strcmp(optarg, "l") == 0 || strcmp(optarg, "local") == 0)
					pfs_mode = false;
				else if (strcmp(optarg, "p") == 0 || strcmp(optarg, "pfs") == 0)
					pfs_mode = true;
				else
				{
					fprintf(stderr,
							_("invalid storage mode \"%s\", must be \"local\" or \"pfs\"\n"), optarg);
					goto end;
				}
				break;
			case 'n':
				polar_disk_name = pg_strdup(optarg);
				break;
			case 'c':
				polar_storage_cluster_name = pg_strdup(optarg);
				break;
			case 'i':
				polar_hostid = atoi(optarg);
				break;
			case '?':
				usage();
				exit(0);
				break;
			default:
				usage();
				goto end;
		}
	}

	if (file_path == NULL)
	{
		fprintf(stderr, "datamax meta file path is NULL, need to be specified\n");
		goto end;
	}

	/* check parameter for pfs mode */
	if (pfs_mode)
	{
		/*
		 * default value of polar_storage_cluster_name is polarstore if not
		 * specified, so it can be NULL
		 */
		if (polar_hostid <= 0 || polar_disk_name == NULL)
		{
			fprintf(stderr, "polar_hostid and polar_disk_name need to be specified in pfs mode\n");
			goto end;
		}
	}

	/*
	 * init fs
	 *
	 * polar_vfs_init_fe() returns early unless
	 * polar_enable_shared_storage_mode is true, so flip that flag for pfs
	 * mode; the polar_open/read/close inlines route via polar_vfs_switch
	 * which defaults to LOCAL.
	 */
	polar_enable_shared_storage_mode = pfs_mode;
	polar_vfs_init_fe(pfs_mode, "datamax_meta", polar_storage_cluster_name, polar_disk_name, POLAR_VFS_RD);
	init_fs = true;

	/* dump meta */
	if (polar_dump_datamax_meta(file_path) == 0)
		succeed = true;
end:
	if (init_fs)
		polar_vfs_destroy_fe("datamax_meta", polar_disk_name);
	return succeed ? 0 : -1;
}

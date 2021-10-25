#include <string.h>
#include <ctype.h>
#include <sys/vfs.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include "shared/os-compatibility.h"
#include "app-info.h"

static char *
app_id_from_flatpak_info(int flatpak_info_fd)
{
	/* would be nice to reuse weston_config_parse */
	FILE *fp;
	char line[2048], *p;
	int app_section = 0, i;

	fp = fdopen(flatpak_info_fd, "r");
	if (fp == NULL)
		return NULL;

	while (fgets(line, sizeof line, fp)) {
		switch (line[0]) {
		case '#':
		case '\n':
			continue;
		case '[':
			p = strchr(&line[1], ']');
			if (!p || p[1] != '\n') {
				fprintf(stderr, "malformed "
					"section header: %s\n", line);
				fclose(fp);
				return NULL;
			}
			p[0] = '\0';
			if (strcmp(&line[1], "Application") == 0)
				app_section = 1;
			else
				app_section = 0;
			continue;
		default:
			p = strchr(line, '=');
			if (!p || p == line) {
				fprintf(stderr, "malformed "
					"config line: %s\n", line);
				fclose(fp);
				return NULL;
			}

			p[0] = '\0';
			p++;
			while (isspace(*p))
				p++;
			i = strlen(p);
			while (i > 0 && isspace(p[i - 1])) {
				p[i - 1] = '\0';
				i--;
			}
			if (app_section && strcmp(line, "name") == 0) {
				fclose(fp);
				return strdup(p);
			}
			continue;
		}
	}

	fclose(fp);
	return NULL;
}

/* stolen from systemd */
/* Returns the number of chars needed to format variables of the
 * specified type as a decimal string. Adds in extra space for a
 * negative '-' prefix (hence works correctly on signed
 * types). Includes space for the trailing NUL. */
#define DECIMAL_STR_MAX(type)                                           \
        (2U+(sizeof(type) <= 1 ? 3U :                                   \
             sizeof(type) <= 2 ? 5U :                                   \
             sizeof(type) <= 4 ? 10U :                                  \
             sizeof(type) <= 8 ? 20U : sizeof(int[-2*(sizeof(type) > 8)])))

#define STRLEN(x) (sizeof(""x"") - sizeof(typeof(x[0])))

void
weston_app_info_find_flatpak(pid_t pid, int pidfd, struct weston_client_app_info *app_info)
{
        char root_path[STRLEN("/proc/") + DECIMAL_STR_MAX(int) + STRLEN("/root")];
        int root_fd = -1;
        int info_fd = -1;
        struct stat stat_buf;
	struct statfs statfs_buf;
	int ret;

	if (pidfd == -1)
		goto err;

	/* stolen from xdg-desktop-portal */
	sprintf(root_path, "/proc/%u/root", pid);
	root_fd = openat(AT_FDCWD, root_path, O_RDONLY | O_NONBLOCK | O_DIRECTORY | O_CLOEXEC | O_NOCTTY);
	if (root_fd == -1) {
		if (errno != EACCES)
			goto err;

		/* Access to the root dir isn't allowed. This can happen if the root is on a fuse
		* filesystem, such as in a toolbox container. We will never have a fuse rootfs
		* in the flatpak case.
		*/
		if (statfs (root_path, &statfs_buf) == 0 &&
		    statfs_buf.f_type == 0x65735546) { /* FUSE_SUPER_MAGIC */
			app_info->kind = WESTON_CLIENT_APP_INFO_KIND_HOST;
			goto check_pid;
		}
	}

	info_fd = openat(root_fd, ".flatpak-info", O_RDONLY | O_CLOEXEC | O_NOCTTY);
	close(root_fd);

	if (info_fd == -1) {
		if (errno != ENOENT)
			goto err;

		/* No file => on the host */
		app_info->kind = WESTON_CLIENT_APP_INFO_KIND_HOST;
		goto check_pid;
	}

	if (fstat(info_fd, &stat_buf) != 0 || !S_ISREG(stat_buf.st_mode)) {
		/* Some weird fd => failure */
		goto err2;
	}

	app_info->kind = WESTON_CLIENT_APP_INFO_KIND_FLATPAK;
	app_info->app_id = app_id_from_flatpak_info(info_fd);
	if (!app_info->app_id)
		goto err2;

	close(info_fd);

check_pid:
	ret = sys_pidfd_send_signal(pidfd, 0, NULL, 0);
	if (ret >= 0 || errno == EPERM)
		return;
	goto err;

err2:
	close(info_fd);
err:
	app_info->kind = WESTON_CLIENT_APP_INFO_KIND_UNKNOWN;
	if (app_info->app_id)
		free(app_info->app_id);
}

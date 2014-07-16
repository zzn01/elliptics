#include "backend.h"
#include <memory>
#include <fcntl.h>
#include "elliptics.h"
#include "../monitor/monitor.h"

static int dnet_ids_generate(struct dnet_node *n, const char *file, unsigned long long storage_free)
{
	int fd, err, size = 1024, i, num;
	struct dnet_raw_id id;
	struct dnet_raw_id raw;
	unsigned long long q = 100 * 1024 * 1024 * 1024ULL;
	char *buf;

	srand(time(NULL) + (unsigned long)n + (unsigned long)file + (unsigned long)&buf);

	fd = open(file, O_RDWR | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC, 0644);
	if (fd < 0) {
		err = -errno;
		dnet_log_err(n, "failed to open/create ids file '%s'", file);
		goto err_out_exit;
	}

	buf = reinterpret_cast<char *>(malloc(size));
	if (!buf) {
		err = -ENOMEM;
		goto err_out_close;
	}
	memset(buf, 0, size);

	num = storage_free / q + 1;
	for (i=0; i<num; ++i) {
		int r = rand();
		memcpy(buf, &r, sizeof(r));

		dnet_transform_node(n, buf, size, id.id, sizeof(id.id));
		memcpy(&raw, id.id, sizeof(struct dnet_raw_id));

		err = write(fd, &raw, sizeof(struct dnet_raw_id));
		if (err != sizeof(struct dnet_raw_id)) {
			dnet_log_err(n, "failed to write id into ids file '%s'", file);
			goto err_out_unlink;
		}
	}

	free(buf);
	close(fd);
	return 0;

err_out_unlink:
	unlink(file);
	free(buf);
err_out_close:
	close(fd);
err_out_exit:
	return err;
}

static struct dnet_raw_id *dnet_ids_init(struct dnet_node *n, const char *hdir, int *id_num, unsigned long long storage_free, struct dnet_addr *cfg_addrs, size_t backend_id)
{
	int fd, err, num;
	const char *file = "ids";
	char path[strlen(hdir) + 1 + strlen(file) + 1]; /* / + null-byte */
	struct stat st;
	struct dnet_raw_id *ids;

	snprintf(path, sizeof(path), "%s/%s", hdir, file);

again:
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		err = -errno;
		if (err == -ENOENT) {
			if (n->flags & DNET_CFG_KEEPS_IDS_IN_CLUSTER)
				err = dnet_ids_update(n, 1, path, cfg_addrs, backend_id);
			if (err)
				err = dnet_ids_generate(n, path, storage_free);

			if (err)
				goto err_out_exit;

			goto again;
		}

		dnet_log_err(n, "failed to open ids file '%s'", path);
		goto err_out_exit;
	}

	err = fstat(fd, &st);
	if (err)
		goto err_out_close;

	if (st.st_size % sizeof(struct dnet_raw_id)) {
		dnet_log(n, DNET_LOG_ERROR, "Ids file size (%lu) is wrong, must be modulo of raw ID size (%zu).\n",
				(unsigned long)st.st_size, sizeof(struct dnet_raw_id));
		goto err_out_close;
	}

	num = st.st_size / sizeof(struct dnet_raw_id);
	if (!num) {
		dnet_log(n, DNET_LOG_ERROR, "No ids read, exiting.\n");
		err = -EINVAL;
		goto err_out_close;
	}

	if (n->flags & DNET_CFG_KEEPS_IDS_IN_CLUSTER)
		dnet_ids_update(n, 0, path, cfg_addrs, backend_id);

	ids = reinterpret_cast<struct dnet_raw_id *>(malloc(st.st_size));
	if (!ids) {
		err = -ENOMEM;
		goto err_out_close;
	}

	err = read(fd, ids, st.st_size);
	if (err != st.st_size) {
		err = -errno;
		dnet_log_err(n, "Failed to read ids file '%s'", path);
		goto err_out_free;
	}

	close(fd);

	*id_num = num;
	return ids;

err_out_free:
	free(ids);
err_out_close:
	close(fd);
err_out_exit:
	return NULL;
}

static const char* dnet_backend_stat_json(void *priv)
{
	struct dnet_backend_callbacks* cb = (struct dnet_backend_callbacks*) priv;

	char* json_stat;
	size_t size;

	cb->storage_stat_json(cb->command_private, &json_stat, &size);
	return json_stat;
}

static void dnet_backend_stat_stop(void *priv)
{
	(void) priv;
}

static int dnet_backend_stat_check_category(void *priv, int category)
{
	(void) priv;
	return category == DNET_MONITOR_BACKEND || category == DNET_MONITOR_ALL;
}

static int dnet_backend_stat_provider_init(struct dnet_backend_io *backend, struct dnet_node *n)
{
	struct stat_provider_raw stat_provider;
	stat_provider.stat_private = backend->cb;
	stat_provider.json = &dnet_backend_stat_json;
	stat_provider.stop = &dnet_backend_stat_stop;
	stat_provider.check_category = &dnet_backend_stat_check_category;
	dnet_monitor_add_provider(n, stat_provider, "backend");
	return 0;
}

int dnet_backend_init(struct dnet_node *node, size_t backend_id)
{
	auto &backends = node->config_data->backends->backends;
	if (backends.size() <= backend_id) {
		dnet_log(node, DNET_LOG_ERROR, "backend_init: backend: %zu, invalid backend id", backend_id);
		return -EINVAL;
	}

	dnet_backend_info &backend = backends[backend_id];
	backend.config = backend.config_template;
	backend.data.assign(backend.data.size(), '\0');
	backend.config.data = backend.data.data();
	backend.config.log = backend.log;

	dnet_backend_io *backend_io = reinterpret_cast<dnet_backend_io *>(malloc(sizeof(dnet_backend_io)));
	if (!backend_io) {
		dnet_log(node, DNET_LOG_ERROR, "backend_init: backend: %zu, failed to allocate memory for dnet_backend_io", backend_id);
		return -ENOMEM;
	}
	std::unique_ptr<dnet_backend_io, free_destroyer> backend_io_guard(backend_io);
	memset(backend_io, 0, sizeof(dnet_backend_io));

	backend_io->backend_id = backend_id;
	backend_io->io = node->io;
	backend_io->cb = &backend.config.cb;

	for (auto it = backend.options.begin(); it != backend.options.end(); ++it) {
		dnet_backend_config_entry &entry = *it;
		entry.value.assign(entry.value_template.begin(), entry.value_template.end());
		entry.entry->callback(&backend.config, entry.entry->key, entry.value.data());
	}

	int err = backend.config.init(&backend.config);
	if (err) {
		dnet_log(node, DNET_LOG_ERROR, "backend_init: backend: %zu, failed to init backend: %d", backend_id, err);
		return err;
	}

	err = dnet_cache_init(node, backend_io);
	if (err) {
		dnet_log(node, DNET_LOG_ERROR, "backend_init: backend: %zu, failed to init cache, err: %d", backend_id, err);
		return err;
	}

	err = dnet_backend_stat_provider_init(backend_io, node);
	if (err) {
		dnet_log(node, DNET_LOG_ERROR, "backend_init: backend: %zu, failed to init stat provider, err: %d", backend_id, err);
		return err;
	}

	err = dnet_backend_io_init(node, backend_io);
	if (err) {
		dnet_log(node, DNET_LOG_ERROR, "backend_init: backend: %zu, failed to init io pool, err: %d", backend_id, err);
		return err;
	}

	{
		dnet_pthread_lock_guard lock_guard(node->io->backends_lock);

		if (node->io->backends_count < backends.size()) {
			size_t backends_count = backends.size();
			node->io->backends = reinterpret_cast<dnet_backend_io **>(malloc(backends_count * sizeof(dnet_backend_io *)));
			if (!node->io->backends) {
				dnet_log(node, DNET_LOG_ERROR, "backend_init: backend: %zu, failed to allocate memory for backends", backend_id);
				return -ENOMEM;
			}

			memset(node->io->backends, 0, backends_count * sizeof(dnet_backend_io *));
			node->io->backends_count = backends_count;
		}

		node->io->backends[backend_id] = backend_io_guard.release();
	}

	int ids_num = 0;
	struct dnet_raw_id *ids = dnet_ids_init(node, backend.history.c_str(), &ids_num, backend.config.storage_free, node->addrs, backend_id);
	err = dnet_route_list_enable_backend(node->route, backend_id, backend.group, ids, ids_num);
	free(ids);

	if (err) {
		dnet_log(node, DNET_LOG_ERROR, "backend_init: backend: %zu, failed to add backend to route list, err: %d", backend_id, err);
		return err;
	}

	return err;
}

void dnet_backend_cleanup(struct dnet_node *node, size_t backend_id)
{
	dnet_backend_io *backend_io = NULL;

	{
		dnet_pthread_lock_guard lock_guard(node->io->backends_lock);
		if (node->io->backends_count > backend_id) {
			backend_io = node->io->backends[backend_id];
			node->io->backends[backend_id] = NULL;
		}
	}

	if (!backend_io)
		return;
}
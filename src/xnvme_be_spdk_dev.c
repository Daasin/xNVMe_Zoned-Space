#include <xnvme_be.h>
#include <xnvme_be_nosys.h>
#ifdef XNVME_BE_SPDK_ENABLED
#include <errno.h>
#include <libxnvme.h>
#include <libxnvme_znd.h>

#include <sys/types.h>
#include <unistd.h>
#include <rte_log.h>
#include <spdk/log.h>
#include <spdk/nvme_spec.h>

#include <xnvme_be.h>
#include <xnvme_queue.h>
#include <xnvme_be_spdk.h>
#include <xnvme_dev.h>
#include <xnvme_sgl.h>
#include <libxnvme_spec.h>
#include <libxnvme_adm.h>

#define XNVME_BE_SPDK_MAX_PROBE_ATTEMPTS 1
#define XNVME_BE_SPDK_AVLB_TRANSPORTS    3

#define XNVME_BE_SPDK_CREFS_LEN 100
static struct xnvme_be_spdk_ctrlr_ref g_cref[XNVME_BE_SPDK_CREFS_LEN];

/**
 * look for a cref for the given given ident
 *
 * @return On success, a pointer to the cref is returned. When none existsWhen a
 * ref is found, 1 is returned cref is assigned, 0 otherwise.
 */
static struct spdk_nvme_ctrlr *
_cref_lookup(struct xnvme_ident *id)
{
	for (int i = 0; i < XNVME_BE_SPDK_CREFS_LEN; ++i) {
		if (strncmp(g_cref[i].uri, id->uri, XNVME_IDENT_URI_LEN - 1)) {
			continue;
		}
		if (!g_cref[i].ctrlr) {
			XNVME_DEBUG("FAILED: corrupted ctrlr in cref");
			return NULL;
		}
		if (g_cref[i].refcount < 1) {
			XNVME_DEBUG("FAILED: corrupted refcount");
			return NULL;
		}

		g_cref[i].refcount += 1;

		return g_cref[i].ctrlr;
	}

	return NULL;
}

/**
 * Insert the given controller
 *
 * @return On success, 0 is return. On error, negated ``errno`` is return to
 * indicate the error.
 */
static int
_cref_insert(struct xnvme_ident *ident, struct spdk_nvme_ctrlr *ctrlr)
{
	if (!ctrlr) {
		XNVME_DEBUG("FAILED: !ctrlr");
		return -EINVAL;
	}

	for (int i = 0; i < XNVME_BE_SPDK_CREFS_LEN; ++i) {
		if (g_cref[i].refcount) {
			continue;
		}

		g_cref[i].ctrlr = ctrlr;
		g_cref[i].refcount += 1;
		strncpy(g_cref[i].uri, ident->uri, XNVME_IDENT_URI_LEN);

		return 0;
	}

	return -ENOMEM;
}

int
_cref_deref(struct spdk_nvme_ctrlr *ctrlr)
{
	if (!ctrlr) {
		XNVME_DEBUG("FAILED: !ctrlr");
		return -EINVAL;
	}

	for (int i = 0; i < XNVME_BE_SPDK_CREFS_LEN; ++i) {
		if (g_cref[i].ctrlr != ctrlr) {
			continue;
		}

		if (g_cref[i].refcount < 1) {
			XNVME_DEBUG("FAILED: invalid refcount: %d",
				    g_cref[i].refcount);
			return -EINVAL;
		}

		g_cref[i].refcount -= 1;

		if (g_cref[i].refcount == 0) {
			XNVME_DEBUG("INFO: refcount: %d => detaching",
				    g_cref[i].refcount);
			spdk_nvme_detach(ctrlr);
			memset(&g_cref[i], 0, sizeof g_cref[i]);
		}

		return 0;
	}

	XNVME_DEBUG("FAILED: no tracking for %p", (void *)ctrlr);
	return -EINVAL;
}

static int g_xnvme_be_spdk_transport[] = {
#ifdef XNVME_BE_SPDK_TRANSPORT_PCIE_ENABLED
	SPDK_NVME_TRANSPORT_PCIE,
#endif
#ifdef XNVME_BE_SPDK_TRANSPORT_TCP_ENABLED
	SPDK_NVME_TRANSPORT_TCP,
#endif
#ifdef XNVME_BE_SPDK_TRANSPORT_RDMA_ENABLED
	SPDK_NVME_TRANSPORT_RDMA,
#endif
#ifdef XNVME_BE_SPDK_TRANSPORT_FC_ENABLED
	SPDK_NVME_TRANSPORT_FC,
#endif
};
static int g_xnvme_be_spdk_ntransport = sizeof g_xnvme_be_spdk_transport / \
					sizeof * g_xnvme_be_spdk_transport;

static int g_xnvme_be_spdk_env_is_initialized = 0;

/**
 * Choke stdout, this is used to choke the output from spdk_env_init
 */
static void
xnvme_stdout_choker(int action)
{
	static int XNVME_STDOUT_FD = 0;
	static FILE *XNVME_STDOUT_FNULL = NULL;

	switch (action) {
	case 1:
		XNVME_STDOUT_FD = dup(STDOUT_FILENO);

		fflush(stdout);
		XNVME_STDOUT_FNULL = fopen("/dev/null", "w");
		dup2(fileno(XNVME_STDOUT_FNULL), STDOUT_FILENO);
		break;
	case 0:
		dup2(XNVME_STDOUT_FD, STDOUT_FILENO);
		close(XNVME_STDOUT_FD);
		break;
	}
}

static void
xnvme_stderr_choker(int action)
{
	static int XNVME_STDERR_FD = 0;
	static FILE *XNVME_STDERR_FNULL = NULL;

	switch (action) {
	case 1:
		XNVME_STDERR_FD = dup(STDERR_FILENO);

		fflush(stderr);
		XNVME_STDERR_FNULL = fopen("/dev/null", "w");
		dup2(fileno(XNVME_STDERR_FNULL), STDERR_FILENO);
		break;
	case 0:
		dup2(XNVME_STDERR_FD, STDERR_FILENO);
		close(XNVME_STDERR_FD);
		break;
	}
}

static void
xnvme_spdk_choker_on(void)
{
#ifdef XNVME_DEBUG_ENABLED
	spdk_log_set_print_level(SPDK_LOG_DEBUG);
	rte_log_set_global_level(RTE_LOG_DEBUG);
	return;
#endif

	xnvme_stdout_choker(1);
	xnvme_stderr_choker(1);

	spdk_log_set_print_level(SPDK_LOG_DISABLED);
	rte_log_set_global_level(RTE_LOG_EMERG);
}

static void
xnvme_spdk_choker_off(void)
{
#ifdef XNVME_DEBUG_ENABLED
	return;
#endif

	xnvme_stdout_choker(0);
	xnvme_stderr_choker(0);
}

static inline int
_spdk_env_init(struct spdk_env_opts *opts)
{
	struct spdk_env_opts _spdk_opts = { 0 };
	int err;

	if (g_xnvme_be_spdk_env_is_initialized) {
		XNVME_DEBUG("INFO: already initialized");
		err = 0;
		goto exit;
	}

#ifdef SPDK_REGHOOK_NVME_PCIE_DRIVER
	XNVME_DEBUG("INFO: SPDK NVMe PCIe Driver registration -- BEGIN");
	if (!spdk_nvme_transport_available_by_name("PCIe")) {
		XNVME_DEBUG("INFO: registering...");
		spdk_reghook_nvme_pcie_driver();
	} else {
		XNVME_DEBUG("INFO: skipping, already registered.");
	}
	XNVME_DEBUG("INFO: SPDK NVMe PCIe Driver registration -- END");
#endif

	xnvme_spdk_choker_on();

	if (!opts) {
		opts = &_spdk_opts;

		spdk_env_opts_init(opts);
		opts->name = "xnvme";
		opts->shm_id = 0;
	}

	err = spdk_env_init(opts);
	if (err < 0) {
		XNVME_DEBUG("FAILED: spdk_env_init(), err: %d", err);
	}

	g_xnvme_be_spdk_env_is_initialized = err == 0;

	xnvme_spdk_choker_off();

exit:
	XNVME_DEBUG("INFO: spdk_env_is_initialized: %d", g_xnvme_be_spdk_env_is_initialized);

	return err;
}

const char *
_spdk_nvmf_adrfam_str(enum spdk_nvmf_adrfam adrfam)
{
	switch (adrfam) {
	case SPDK_NVMF_ADRFAM_IPV4:
		return "IPv4";
	case SPDK_NVMF_ADRFAM_IPV6:
		return "IPv6";
	case SPDK_NVMF_ADRFAM_IB:
	case SPDK_NVMF_ADRFAM_FC:
	case SPDK_NVMF_ADRFAM_INTRA_HOST:
		return "";
	}

	return "";
}

/**
 * Construct a SPDK transport identifier from a xNVMe identifier
 *
 * A trtype must be supplied as xNVMe does not distinguish between TCP, RDMA,
 * etc. it only has the 'fab' prefix for Fabrics transports.
 *
 * TODO:
 * - construct trid for FC
 */
static inline int
_xnvme_be_spdk_ident_to_trid(const struct xnvme_ident *ident, struct xnvme_opts *opts,
			     struct spdk_nvme_transport_id *trid, int trtype)
{
	char trid_str[1024] = { 0 }; // TODO: fix this size
	char addr[SPDK_NVMF_TRADDR_MAX_LEN + 1] = { 0 };
	int port = 0;
	int err;

	memset(trid, 0, sizeof(*trid));

	switch (trtype) {
	case SPDK_NVME_TRANSPORT_PCIE:
		sprintf(trid_str, "trtype:%s traddr:%s",
			spdk_nvme_transport_id_trtype_str(trtype), ident->uri);
		break;

	case SPDK_NVME_TRANSPORT_TCP:
	case SPDK_NVME_TRANSPORT_RDMA:
		if (sscanf(ident->uri, "%[^:]:%d", addr, &port) != 2) {
			XNVME_DEBUG("FAILED: uri: %s, trtype: %s",
				    ident->uri, spdk_nvme_transport_id_trtype_str(trtype));
			return -EINVAL;
		}
		snprintf(trid_str, sizeof(trid_str),
			 "trtype:%s adrfam:%s traddr:%s trsvcid:%d",
			 spdk_nvme_transport_id_trtype_str(trtype),
			 opts->adrfam ? opts->adrfam : "IPv4", addr, port);
		break;

	case SPDK_NVME_TRANSPORT_FC:
	case SPDK_NVME_TRANSPORT_CUSTOM:
		XNVME_DEBUG("FAILED: unsupported trtype: %s",
			    spdk_nvme_transport_id_trtype_str(trtype));
		return -ENOSYS;
	}

	err = spdk_nvme_transport_id_parse(trid, trid_str);
	if (err) {
		XNVME_DEBUG("FAILED: parsing trid_str: %s from uri: %s, err: %d",
			    trid_str, ident->uri, err);
		return err;
	}

	switch (trtype) {
	case SPDK_NVME_TRANSPORT_PCIE:
		break;

	case SPDK_NVME_TRANSPORT_TCP:
	case SPDK_NVME_TRANSPORT_RDMA:
		snprintf(trid->subnqn, sizeof trid->subnqn, "%s", SPDK_NVMF_DISCOVERY_NQN);
		break;
	}

	return 0;
}

int
_spdk_nvme_transport_id_pr(const struct spdk_nvme_transport_id *trid)
{
	int wrtn = 0;

	wrtn += printf("trid:\n");
	wrtn += printf("  trstring: '%s'\n", trid->trstring);
	wrtn += printf("  trtype: 0x%x\n", trid->trtype);
	wrtn += printf("  adrfam: 0x%x\n", trid->adrfam);
	wrtn += printf("  traddr: '%s'\n", trid->traddr);
	wrtn += printf("  trsvcid: '%s'\n", trid->trsvcid);
	wrtn += printf("  subnqn: '%s'\n", trid->subnqn);
	wrtn += printf("  priority: 0x%x\n", trid->priority);

	return wrtn;
}

int
_spdk_nvme_transport_id_compare_weak(const struct spdk_nvme_transport_id *trid1,
				     const struct spdk_nvme_transport_id *trid2)
{
	struct spdk_nvme_transport_id one = *trid1;
	struct spdk_nvme_transport_id other = *trid2;

	memset(one.subnqn, 0, sizeof one.subnqn);
	memset(other.subnqn, 0, sizeof one.subnqn);

	return spdk_nvme_transport_id_compare(&one, &other);
}

/**
 * Attach to the device matching the device provided by the user, and only when
 * it is not already attached, e.g. attach when:
 *
 * (!attached) && (requested == probed)
 *
 * Also, setup controller-options
 */
static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *probed,
	 struct spdk_nvme_ctrlr_opts *ctrlr_opts)
{
	struct xnvme_dev *dev = cb_ctx;
	struct xnvme_be_spdk_state *state = (void *)dev->be.state;
	struct spdk_nvme_transport_id req = { 0 };

	if (state->attached) {
		XNVME_DEBUG("SKIP: Already attached");
		return false;
	}
	if (_xnvme_be_spdk_ident_to_trid(&dev->ident, &dev->opts, &req, probed->trtype)) {
		XNVME_DEBUG("SKIP: !_xnvme_be_spdk_ident_to_trid()");
		return false;
	}
	if (_spdk_nvme_transport_id_compare_weak(probed, &req)) {
		XNVME_DEBUG("SKIP: mismatching trid prbed != ctx");

		_spdk_nvme_transport_id_pr(probed);
		_spdk_nvme_transport_id_pr(&req);

		return false;
	}

	// Setup controller options, general as well as trtype-specific
	ctrlr_opts->command_set = dev->opts.css.given ? dev->opts.css.value : SPDK_NVME_CC_CSS_IOCS;

	switch (req.trtype) {
	case SPDK_NVME_TRANSPORT_PCIE:
		ctrlr_opts->use_cmb_sqs = dev->opts.use_cmb_sqs ? true : false;
		break;

	case SPDK_NVME_TRANSPORT_TCP:
	case SPDK_NVME_TRANSPORT_RDMA:
		ctrlr_opts->header_digest = 1;
		ctrlr_opts->data_digest = 1;
		ctrlr_opts->keep_alive_timeout_ms = 0;
		break;

	case SPDK_NVME_TRANSPORT_VFIOUSER:
	case SPDK_NVME_TRANSPORT_FC:
	case SPDK_NVME_TRANSPORT_CUSTOM:
		XNVME_DEBUG("FAILED: unsupported trtype: %d", probed->trtype);
		return false;
	}

#ifdef XNVME_DEBUG_ENABLED
	_spdk_nvme_transport_id_pr(probed);
#endif

	return true;
}

/**
 * Sets up the state{ns, ctrlr, attached} given via the cb_ctx
 * detached if dev->nsid is not a match
 */
static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid, struct spdk_nvme_ctrlr *ctrlr,
	  const struct spdk_nvme_ctrlr_opts *XNVME_UNUSED(ctrlr_opts))
{
	struct xnvme_dev *dev = cb_ctx;
	struct xnvme_opts *opts = &dev->opts;
	struct xnvme_be_spdk_state *state = (void *)dev->be.state;
	struct spdk_nvme_ns *ns = NULL;

	XNVME_DEBUG("INFO: nsid: %d", opts->nsid);

	ns = spdk_nvme_ctrlr_get_ns(ctrlr, opts->nsid);
	if (!ns) {
		XNVME_DEBUG("FAILED: spdk_nvme_ctrlr_get_ns(0x%x)", opts->nsid);
		spdk_nvme_detach(ctrlr);
		return;
	}
	if (!spdk_nvme_ns_is_active(ns)) {
		XNVME_DEBUG("FAILED: !spdk_nvme_ns_is_active(opts->nsid:0x%x)",
			    opts->nsid);
		spdk_nvme_detach(ctrlr);
		return;
	}

	state->ns = ns;
	state->ctrlr = ctrlr;
	state->attached = 1;
	opts->spdk_fabrics = trid->trtype > SPDK_NVME_TRANSPORT_PCIE;
}

void
xnvme_be_spdk_state_term(struct xnvme_be_spdk_state *state)
{
	int err;

	if (!state) {
		return;
	}
	if (state->qpair) {
		spdk_nvme_ctrlr_free_io_qpair(state->qpair);
		err = pthread_mutex_destroy(&state->qpair_lock);
		if (err) {
			printf("UNHANDLED: pthread_mutex_destroy(): '%s'\n",
			       strerror(err));
		}
	}
	err = _cref_deref(state->ctrlr);
	if (err) {
		XNVME_DEBUG("FAILED: _cref_deref():, err: %d", err);
	}
}

void
xnvme_be_spdk_dev_close(struct xnvme_dev *dev)
{
	if (!dev) {
		return;
	}

	xnvme_be_spdk_state_term((void *)dev->be.state);
	memset(&dev->be, 0, sizeof(dev->be));
}

struct xnvme_be_spdk_enumerate_ctx {
	struct xnvme_opts *opts;
	xnvme_enumerate_cb enumerate_cb;
	void *cb_args;
};

/**
 * Always attach to supported transports such that `enumerate_probe_cb` can
 * grab namespaces
 *
 * And disable CMB sqs / cqs for now due to shared PMR / CMB
 */
static bool
enumerate_probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
		   struct spdk_nvme_ctrlr_opts *ctrlr_opts)
{
	struct xnvme_be_spdk_enumerate_ctx *ectx = cb_ctx;
	struct xnvme_opts *opts = ectx->opts;

	// Setup controller options, general as well as trtype-specific
	ctrlr_opts->command_set = opts->css.given ? opts->css.value : SPDK_NVME_CC_CSS_IOCS;

	switch (trid->trtype) {
	case SPDK_NVME_TRANSPORT_PCIE:
		ctrlr_opts->use_cmb_sqs = opts->use_cmb_sqs ? true : false;
		break;

	case SPDK_NVME_TRANSPORT_TCP:
	case SPDK_NVME_TRANSPORT_RDMA:
		ctrlr_opts->header_digest = 1;
		ctrlr_opts->data_digest = 1;
		ctrlr_opts->keep_alive_timeout_ms = 0;
		break;

	case SPDK_NVME_TRANSPORT_VFIOUSER:
	case SPDK_NVME_TRANSPORT_FC:
	case SPDK_NVME_TRANSPORT_CUSTOM:
		XNVME_DEBUG("FAILED: unsupported trtype: %d", trid->trtype);
		return false;
	}

	return true;
}

static void
enumerate_attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
		    struct spdk_nvme_ctrlr *ctrlr,
		    const struct spdk_nvme_ctrlr_opts *ctrlr_opts)
{
	struct xnvme_be_spdk_enumerate_ctx *ectx = cb_ctx;
	const int num_ns = spdk_nvme_ctrlr_get_num_ns(ctrlr);

	for (int nsid = 1; nsid <= num_ns; ++nsid) {
		struct xnvme_opts opts = *ectx->opts;
		struct xnvme_ident ident = { 0 };
		struct xnvme_dev *dev = NULL;
		struct spdk_nvme_ns *ns;
		int err;

		// Option options based on what the driver ended up using
		// NOTE: we communicate the css via the dev->ident.csi
		opts.use_cmb_sqs = ctrlr_opts->use_cmb_sqs;
		opts.nsid = nsid;
		opts.spdk_fabrics = trid->trtype > SPDK_NVME_TRANSPORT_PCIE;

		if (!spdk_nvme_ctrlr_get_data(ctrlr)) {
			XNVME_DEBUG("FAILED: spdk_nvme_ctrlr_get_data");
			continue;
		}
		ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
		if (!ns) {
			continue;
		}
		if (!spdk_nvme_ns_is_active(ns)) {
			XNVME_DEBUG("FAILED: skipping inactive nsid: %d", nsid);
			continue;
		}
		if (!spdk_nvme_ns_get_data(ns)) {
			XNVME_DEBUG("FAILED: spdk_nvme_ns_get_data()");
			continue;
		}

		ident.dtype = XNVME_DEV_TYPE_NVME_NAMESPACE;
		ident.nsid = nsid;
		ident.csi = spdk_nvme_ns_get_csi(ns);

		// Namespace looks good, construct xNVMe identifier, and ..
		switch (trid->trtype) {
		case SPDK_NVME_TRANSPORT_PCIE:
			snprintf(ident.uri, sizeof(ident.uri), "%s", trid->traddr);
			break;

		case SPDK_NVME_TRANSPORT_TCP:
		case SPDK_NVME_TRANSPORT_RDMA:
			snprintf(ident.uri, sizeof(ident.uri), "%s:%s",
				 trid->traddr, trid->trsvcid);
			break;

		case SPDK_NVME_TRANSPORT_FC:
		case SPDK_NVME_TRANSPORT_CUSTOM:
		case SPDK_NVME_TRANSPORT_VFIOUSER:
			XNVME_DEBUG("SKIP: ENOSYS trtype: %s",
				    spdk_nvme_transport_id_trtype_str(trid->trtype));
			continue;
		}

		// Save the reference to ctrlr so it can be reused when we call xnvme_dev_open()
		err = _cref_insert(&ident, ctrlr);
		if (err) {
			XNVME_DEBUG("FAILED: _cref_insert(), err: %d", err);
			return;
		}

		dev = xnvme_dev_open(ident.uri, &opts);
		if (!dev) {
			XNVME_DEBUG("FAILED: xnvme_dev_open()%d", errno);
			return;
		}
		if (ectx->enumerate_cb(dev, ectx->cb_args)) {
			xnvme_dev_close(dev);
		}
	}

	if (spdk_nvme_detach(ctrlr)) {
		XNVME_DEBUG("FAILED: spdk_nvme_detach()");
	}
}

/**
 * TODO
 *
 * - Fix size of trid_str
 * - Make use of '?addrfam' for TCP  + RDMA transport when available in sys_uri
 * - Add enumerate FC / construction of identifiers
 * - Consider how to support enumerating custom transports
 */
int
xnvme_be_spdk_enumerate(const char *sys_uri, struct xnvme_opts *opts, xnvme_enumerate_cb cb_func,
			void *cb_args)
{
	struct xnvme_be_spdk_enumerate_ctx ectx = { 0 };
	char addr[SPDK_NVMF_TRADDR_MAX_LEN + 1] = { 0 };
	struct xnvme_ident ident = { 0 };
	int port = 0;

	if (_spdk_env_init(NULL)) {
		XNVME_DEBUG("FAILED: _spdk_env_init()");
		return -ESRCH;
	}
	if (sys_uri) {
		if (xnvme_ident_from_uri(sys_uri, &ident)) {
			XNVME_DEBUG("FAILED: ident_from_uri, uri: %s", sys_uri);
			return -EINVAL;
		}
		if (sscanf(ident.uri, "%[^:]:%d", addr, &port) != 2) {
			XNVME_DEBUG("FAILED: invalid sys_uri: %s", sys_uri);
			return -EINVAL;
		}
	}
	XNVME_DEBUG("INFO: addr: %s, port: %d", addr, port);

	ectx.opts = opts;
	ectx.enumerate_cb = cb_func;
	ectx.cb_args = cb_args;

	for (int t = 0; t < g_xnvme_be_spdk_ntransport; ++t) {
		int trtype = g_xnvme_be_spdk_transport[t];
		struct spdk_nvme_transport_id trid = { 0 };
		char trid_str[1024] = { 0 };
		int err;

		XNVME_DEBUG("INFO: trtype: %s, # %d of %d",
			    spdk_nvme_transport_id_trtype_str(trtype),
			    t + 1, g_xnvme_be_spdk_ntransport);

		switch (trtype) {
		case SPDK_NVME_TRANSPORT_PCIE:
			trid.trtype = trtype;
			break;

		case SPDK_NVME_TRANSPORT_TCP:
		case SPDK_NVME_TRANSPORT_RDMA:
			if (!sys_uri) {
				XNVME_DEBUG("SKIP: !sys_uri");
				continue;
			}

			snprintf(trid_str, sizeof trid_str,
				 "trtype:%s adrfam:%s traddr:%s trsvcid:%d",
				 spdk_nvme_transport_id_trtype_str(trtype),
				 opts->adrfam ? opts->adrfam : "IPv4", addr, port);

			err = spdk_nvme_transport_id_parse(&trid, trid_str);
			if (err) {
				XNVME_DEBUG("FAILED: id_parse(): %s, err: %d",
					    trid_str, err);
				continue;
			}
			snprintf(trid.subnqn, sizeof trid.subnqn, "%s", SPDK_NVMF_DISCOVERY_NQN);
			break;

		case SPDK_NVME_TRANSPORT_FC:
		case SPDK_NVME_TRANSPORT_CUSTOM:
			XNVME_DEBUG("SKIP: enum. of trtype: %s not implemented",
				    spdk_nvme_transport_id_trtype_str(trtype));
			continue;
		}

		err = spdk_nvme_probe(&trid, &ectx, enumerate_probe_cb,
				      enumerate_attach_cb, NULL);
		if (err) {
			XNVME_DEBUG("FAILED: spdk_nvme_probe(), err: %d", err);
		}
	}

	return 0;
}

/**
 * - Parse options from dev->ident
 * - Initialize SPDK environment
 * - Attach to controller matching dev->ident
 * - create sync-io-qpair
 * - create lock protecting sync-io-qpair
 */
int
xnvme_be_spdk_state_init(struct xnvme_dev *dev)
{
	struct xnvme_be_spdk_state *state = (void *)dev->be.state;
	struct spdk_env_opts env_opts = { 0 };
	int err;

	if (!dev->opts.nsid) {
		XNVME_DEBUG("FAILED: dev->opts.nsid: %d, ", dev->opts.nsid);
		return -EINVAL;
	}

	spdk_env_opts_init(&env_opts);

	if (dev->opts.core_mask) {
		XNVME_DEBUG("INFO: multi-process setup");
		env_opts.shm_id = dev->opts.shm_id;
		env_opts.core_mask = dev->opts.core_mask;
		env_opts.main_core = dev->opts.main_core;
	}

	err = _spdk_env_init(&env_opts);
	if (err) {
		XNVME_DEBUG("FAILED: _spdk_env_init(), err: %d", err);
		return err;
	}

	state->ctrlr = _cref_lookup(&dev->ident);
	if (state->ctrlr) {
		struct spdk_nvme_ns *ns;

		XNVME_DEBUG("INFO: found dev->ident.uri: '%s' via cref_lookup()", dev->ident.uri);

		ns = spdk_nvme_ctrlr_get_ns(state->ctrlr, dev->opts.nsid);
		if (!ns) {
			XNVME_DEBUG("FAILED: spdk_nvme_ctrlr_get_ns(0x%x)", dev->opts.nsid);
			if (_cref_deref(state->ctrlr)) {
				XNVME_DEBUG("FAILED: _cref_deref");
			}
			goto probe;
		}
		if (!spdk_nvme_ns_is_active(ns)) {
			XNVME_DEBUG("FAILED: !spdk_nvme_ns_is_active(nsid:0x%x)", dev->opts.nsid);
			if (_cref_deref(state->ctrlr)) {
				XNVME_DEBUG("FAILED: _cref_deref");
			}
			goto probe;
		}

		state->ns = ns;
		state->attached = 1;
		XNVME_DEBUG("INFO: re-using previously attached controller");
	}

probe:
	// Probe for device matching dev->ident
	for (int i = 0; !state->attached; ++i) {
		if (XNVME_BE_SPDK_MAX_PROBE_ATTEMPTS == i) {
			XNVME_DEBUG("FAILED: max attempts exceeded");
			return -ENXIO;
		}

		for (int t = 0; t < g_xnvme_be_spdk_ntransport; ++t) {
			int trtype = g_xnvme_be_spdk_transport[t];
			struct spdk_nvme_transport_id trid = { 0 };

			XNVME_DEBUG("############################");
			XNVME_DEBUG("INFO: trtype: %s, # %d of %d",
				    spdk_nvme_transport_id_trtype_str(trtype),
				    t + 1, g_xnvme_be_spdk_ntransport);

			if (_xnvme_be_spdk_ident_to_trid(&dev->ident, &dev->opts, &trid, trtype)) {
				XNVME_DEBUG("SKIP/FAILED: ident_to_trid()");
				continue;
			}

			err = spdk_nvme_probe(&trid, dev, probe_cb, attach_cb, NULL);
			if ((err) || (!state->attached)) {
				XNVME_DEBUG("FAILED: probe a:%d, e:%d, i:%d",
					    state->attached, err, i);
			}
		}
	}

	dev->ident.dtype = XNVME_DEV_TYPE_NVME_NAMESPACE;
	dev->ident.csi = spdk_nvme_ns_get_csi(state->ns);
	dev->ident.nsid = dev->opts.nsid;

	// Setup IO qpair lock for SYNC commands
	err = pthread_mutex_init(&state->qpair_lock, NULL);
	if (err) {
		XNVME_DEBUG("FAILED: pthread_mutex_init(): '%s'",
			    strerror(err));
		return -err;
	}

	// Setup IO qpair for SYNC commands
	state->qpair = spdk_nvme_ctrlr_alloc_io_qpair(state->ctrlr, NULL, 0);
	if (!state->qpair) {
		XNVME_DEBUG("FAILED: spdk_nvme_ctrlr_alloc_io_qpair()");
		return -ENOMEM;
	}

	err = _cref_insert(&dev->ident, state->ctrlr);
	if (err) {
		XNVME_DEBUG("FAILED: _cref_insert(), err: %d", err);
		return err;
	}

	XNVME_DEBUG("INFO: open() : OK");

	return 0;
}

int
xnvme_be_spdk_dev_open(struct xnvme_dev *dev)
{
	int err;

	err = xnvme_be_spdk_state_init(dev);
	if (err) {
		XNVME_DEBUG("FAILED: xnvme_be_spdk_state_init()");
		return err;
	}
	err = xnvme_be_dev_idfy(dev);
	if (err) {
		XNVME_DEBUG("FAILED: xnvme_be_spdk_dev_idfy()");
		xnvme_be_spdk_state_term((void *)dev->be.state);
		return err;
	}
	err = xnvme_be_dev_derive_geometry(dev);
	if (err) {
		XNVME_DEBUG("FAILED: xnvme_be_dev_derive_geometry()");
		xnvme_be_spdk_state_term((void *)dev->be.state);
		return err;
	}

	return err;
}
#endif

struct xnvme_be_dev g_xnvme_be_spdk_dev = {
#ifdef XNVME_BE_SPDK_ENABLED
	.enumerate = xnvme_be_spdk_enumerate,
	.dev_open = xnvme_be_spdk_dev_open,
	.dev_close = xnvme_be_spdk_dev_close,
#else
	.enumerate = xnvme_be_nosys_enumerate,
	.dev_open = xnvme_be_nosys_dev_open,
	.dev_close = xnvme_be_nosys_dev_close,
#endif
};

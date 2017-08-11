/*
 * Copyright (c) 2015-2017 The Linux Foundation. All rights reserved.
 *
 * Previously licensed under the ISC license by Qualcomm Atheros, Inc.
 *
 *
 * Permission to use, copy, modify, and/or distribute this software for
 * any purpose with or without fee is hereby granted, provided that the
 * above copyright notice and this permission notice appear in all
 * copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * This file was originally distributed by Qualcomm Atheros, Inc.
 * under proprietary terms before Copyright ownership was assigned
 * to the Linux Foundation.
 */

/**
 * DOC: wlan_hdd_napi.c
 *
 * WLAN HDD NAPI interface implementation
 */
#include <smp.h> /* get_cpu */

#include "wlan_hdd_napi.h"
#include "cds_api.h"       /* cds_get_context */
#include "hif.h"           /* hif_map_service...*/
#include "wlan_hdd_main.h" /* hdd_err/warn... */
#include "qdf_types.h"     /* QDF_MODULE_ID_... */
#include "ce_api.h"

/*  guaranteed to be initialized to zero/NULL by the standard */
static struct qca_napi_data *hdd_napi_ctx;

/**
 * hdd_napi_get_all() - return the whole NAPI structure from HIF
 *
 * Gets to the data structure common to all NAPI instances.
 *
 * Return:
 *  NULL  : probably NAPI not initialized yet.
 *  <addr>: the address of the whole NAPI structure
 */
struct qca_napi_data *hdd_napi_get_all(void)
{
	struct qca_napi_data *rp = NULL;
	struct hif_opaque_softc *hif;

	NAPI_DEBUG("-->");

	hif = cds_get_context(QDF_MODULE_ID_HIF);
	if (unlikely(NULL == hif))
		QDF_ASSERT(NULL != hif); /* WARN */
	else
		rp = hif_napi_get_all(hif);

	NAPI_DEBUG("<-- [addr=%p]", rp);
	return rp;
}

/**
 * hdd_napi_get_map() - get a copy of napi pipe map
 *
 * Return:
 *  uint32_t  : copy of pipe map
 */
static uint32_t hdd_napi_get_map(void)
{
	uint32_t map = 0;

	NAPI_DEBUG("-->");
	/* cache once, use forever */
	if (hdd_napi_ctx == NULL)
		hdd_napi_ctx = hdd_napi_get_all();
	if (hdd_napi_ctx != NULL)
		map = hdd_napi_ctx->ce_map;

	NAPI_DEBUG("<-- [map=0x%08x]", map);
	return map;
}

/**
 * hdd_napi_create() - creates the NAPI structures for a given netdev
 *
 * Creates NAPI instances. This function is called
 * unconditionally during initialization. It creates
 * napi structures through the proper HTC/HIF calls.
 * The structures are disabled on creation.
 *
 * Return:
 *   single-queue: <0: err, >0=id, 0 (should not happen)
 *   multi-queue: bitmap of created instances (0: none)
 */
int hdd_napi_create(void)
{
	struct  hif_opaque_softc *hif_ctx;
	int     rc = 0;
	hdd_context_t *hdd_ctx;
	uint8_t feature_flags = 0;

	NAPI_DEBUG("-->");

	hif_ctx = cds_get_context(QDF_MODULE_ID_HIF);
	if (unlikely(NULL == hif_ctx)) {
		QDF_ASSERT(NULL != hif_ctx);
		rc = -EFAULT;
	} else {

		feature_flags = QCA_NAPI_FEATURE_CPU_CORRECTION |
				QCA_NAPI_FEATURE_IRQ_BLACKLISTING |
				QCA_NAPI_FEATURE_CORE_CTL_BOOST;

		rc = hif_napi_create(hif_ctx, hdd_napi_poll,
				     QCA_NAPI_BUDGET,
				     QCA_NAPI_DEF_SCALE,
				     feature_flags);
		if (rc < 0) {
			hdd_err("ERR(%d) creating NAPI instances",
				rc);
		} else {
			hdd_info("napi instances were created. Map=0x%x", rc);
			hdd_ctx = cds_get_context(QDF_MODULE_ID_HDD);
			if (unlikely(NULL == hdd_ctx)) {
				QDF_ASSERT(0);
				rc = -EFAULT;
			} else {
				rc = hdd_napi_event(NAPI_EVT_INI_FILE,
					(void *)hdd_ctx->napi_enable);
			}
		}

	}
	NAPI_DEBUG("<-- [rc=%d]", rc);

	return rc;
}

/**
 * hdd_napi_destroy() - destroys the NAPI structures for a given netdev
 * @force: if set, will force-disable the instance before _del'ing
 *
 * Destroy NAPI instances. This function is called
 * unconditionally during module removal. It destroy
 * napi structures through the proper HTC/HIF calls.
 *
 * Return:
 *    number of NAPI instances destroyed
 */
int hdd_napi_destroy(int force)
{
	int rc = 0;
	int i;
	uint32_t hdd_napi_map = hdd_napi_get_map();

	NAPI_DEBUG("--> (force=%d)", force);
	if (hdd_napi_map) {
		struct hif_opaque_softc *hif_ctx;

		hif_ctx = cds_get_context(QDF_MODULE_ID_HIF);
		if (unlikely(NULL == hif_ctx))
			QDF_ASSERT(NULL != hif_ctx);
		else
			for (i = 0; i < CE_COUNT_MAX; i++)
				if (hdd_napi_map & (0x01 << i)) {
					if (0 <= hif_napi_destroy(
						    hif_ctx,
						    NAPI_PIPE2ID(i), force)) {
						rc++;
						hdd_napi_map &= ~(0x01 << i);
					} else
						hdd_err("cannot destroy napi %d: (pipe:%d), f=%d\n",
							i,
							NAPI_PIPE2ID(i), force);
				}
	}

	/* if all instances are removed, it is likely that hif_context has been
	 * removed as well, so the cached value of the napi context also needs
	 * to be removed
	 */
	if (force)
		QDF_ASSERT(hdd_napi_map == 0);
	if (0 == hdd_napi_map)
		hdd_napi_ctx = NULL;

	NAPI_DEBUG("<-- [rc=%d]", rc);
	return rc;
}

/**
 * hdd_napi_enabled() - checks if NAPI is enabled (for a given id)
 * @id: the id of the NAPI to check (any= -1)
 *
 * Return:
 *   int: 0  = false (NOT enabled)
 *        !0 = true  (enabbled)
 */
int hdd_napi_enabled(int id)
{
	struct hif_opaque_softc *hif;
	int rc = 0; /* NOT enabled */

	hif = cds_get_context(QDF_MODULE_ID_HIF);
	if (unlikely(NULL == hif))
		QDF_ASSERT(hif != NULL); /* WARN_ON; rc = 0 */
	else if (-1 == id)
		rc = hif_napi_enabled(hif, id);
	else
		rc = hif_napi_enabled(hif, NAPI_ID2PIPE(id));
	return rc;
}

/**
 * hdd_napi_event() - relay the event detected by HDD to HIF NAPI event handler
 * @event: event code
 * @data : event-specific auxiliary data
 *
 * See function documentation in hif_napi.c::hif_napi_event for list of events
 * and how each of them is handled.
 *
 * Return:
 *  < 0: error code
 *  = 0: event handled successfully
 */
int hdd_napi_event(enum qca_napi_event event, void *data)
{
	int rc = -EFAULT;  /* assume err */
	struct hif_opaque_softc *hif;

	NAPI_DEBUG("-->(event=%d, aux=%p)", event, data);

	hif = cds_get_context(QDF_MODULE_ID_HIF);
	if (unlikely(NULL == hif))
		QDF_ASSERT(hif != NULL);
	else
		rc = hif_napi_event(hif, event, data);

	NAPI_DEBUG("<--[rc=%d]", rc);
	return rc;
}

#ifdef HELIUMPLUS
/**
 * hdd_napi_apply_throughput_policy() - implement the throughput action policy
 * @hddctx:     HDD context
 * @tx_packets: number of tx packets in the last interval
 * @rx_packets: number of rx packets in the last interval
 *
 * Called by hdd_bus_bw_compute_cb, checks the number of packets in the last
 * interval, and determines the desired napi throughput state (HI/LO). If
 * the desired state is different from the current, then it invokes the
 * event handler to switch to the desired state.
 *
 * The policy implementation is limited to this function and
 * The current policy is: determine the NAPI mode based on the condition:
 *      (total number of packets > medium threshold)
 * - tx packets are included because:
 *   a- tx-completions arrive at one of the rx CEs
 *   b- in TCP, a lof of TX implies ~(tx/2) rx (ACKs)
 *   c- so that we can use the same normalized criteria in ini file
 * - medium-threshold (default: 500 packets / 10 ms), because
 *   we would like to be more reactive.
 *
 * Return: 0 : no action taken, or action return code
 *         !0: error, or action error code
 */
static int napi_tput_policy_delay;
int hdd_napi_apply_throughput_policy(struct hdd_context *hddctx,
				     uint64_t              tx_packets,
				     uint64_t              rx_packets)
{
	int rc = 0;
	uint64_t packets = tx_packets + rx_packets;
	enum qca_napi_tput_state req_state;
	struct qca_napi_data *napid = hdd_napi_get_all();
	int enabled;

	NAPI_DEBUG("-->%s(tx=%lld, rx=%lld)", __func__, tx_packets, rx_packets);

	if (unlikely(napi_tput_policy_delay < 0))
		napi_tput_policy_delay = 0;
	if (napi_tput_policy_delay > 0) {
		NAPI_DEBUG("%s: delaying policy; delay-count=%d",
			  __func__, napi_tput_policy_delay);
		napi_tput_policy_delay--;

		/* make sure the next timer call calls us */
		hddctx->cur_vote_level = -1;

		return rc;
	}

	if (!napid) {
		hdd_err("ERR: napid NULL");
		return rc;
	}

	enabled = hdd_napi_enabled(HDD_NAPI_ANY);
	if (!enabled) {
		hdd_err("ERR: napi not enabled");
		return rc;
	}

	if (packets > hddctx->config->busBandwidthHighThreshold)
		req_state = QCA_NAPI_TPUT_HI;
	else
		req_state = QCA_NAPI_TPUT_LO;

	if (req_state != napid->napi_mode)
		rc = hdd_napi_event(NAPI_EVT_TPUT_STATE, (void *)req_state);
	return rc;
}

/**
 * hdd_napi_serialize() - serialize all NAPI activities
 * @is_on: 1="serialize" or 0="de-serialize"
 *
 * Start/stop "serial-NAPI-mode".
 * NAPI serial mode describes a state where all NAPI operations are forced to be
 * run serially. This is achieved by ensuring all NAPI instances are run on the
 * same CPU, so forced to be serial.
 * NAPI life-cycle:
 * - Interrupt is received for a given CE.
 * - In the ISR, the interrupt is masked and corresponding NAPI instance
 *   is scheduled, to be run as a bottom-half.
 * - Bottom-half starts with a poll call (by the net_rx softirq). There may be
 *   one of more subsequent calls until the work is complete.
 * - Once the work is complete, the poll handler enables the interrupt and
 *   the cycle re-starts.
 *
 * Return: <0: error-code (operation failed)
 *         =0: success
 *         >0: status (not used)
 */
int hdd_napi_serialize(int is_on)
{
	int rc;
	hdd_context_t *hdd_ctx;
#define POLICY_DELAY_FACTOR (1)
	rc = hif_napi_serialize(cds_get_context(QDF_MODULE_ID_HIF), is_on);
	if ((rc == 0) && (is_on == 0)) {
		/* apply throughput policy after one timeout */
		napi_tput_policy_delay = POLICY_DELAY_FACTOR;

		/* make sure that bus_bandwidth trigger is executed */
		hdd_ctx = cds_get_context(QDF_MODULE_ID_HDD);
		if (hdd_ctx != NULL)
			hdd_ctx->cur_vote_level = -1;

	}
	return rc;
}
#endif /* HELIUMPLUS */

/**
 * hdd_napi_poll() - NAPI poll function
 * @napi  : pointer to NAPI struct
 * @budget: the pre-declared budget
 *
 * Implementation of poll function. This function is called
 * by kernel during softirq processing.
 *
 * NOTE FOR THE MAINTAINER:
 *   Make sure this is very close to the ce_tasklet code.
 *
 * Return:
 *   int: the amount of work done ( <= budget )
 */
int hdd_napi_poll(struct napi_struct *napi, int budget)
{
	return hif_napi_poll(cds_get_context(QDF_MODULE_ID_HIF), napi, budget);
}

/**
 * hdd_display_napi_stats() - print NAPI stats
 *
 * Return: == 0: success; !=0: failure
 */
int hdd_display_napi_stats(void)
{
	int i, j, k, n; /* NAPI, CPU, bucket indices, bucket buf write index*/
	int max;
	struct qca_napi_data *napid;
	struct qca_napi_info *napii;
	struct qca_napi_stat *napis;
	/*
	 * Expecting each NAPI bucket item to need at max 5 numerals + space for
	 * formatting. For example "10000 " Thus the array needs to have
	 * (5 + 1) * QCA_NAPI_NUM_BUCKETS bytes of space. Leaving one space at
	 * the end of the "buf" arrary for end of string char.
	 */
	char buf[6 * QCA_NAPI_NUM_BUCKETS + 1] = {'\0'};

	napid = hdd_napi_get_all();
	if (NULL == napid) {
		hdd_err("%s unable to retrieve napi structure", __func__);
		return -EFAULT;
	}
	qdf_print("[NAPI %u][BL %d]:  scheds   polls   comps    done t-lim p-lim  corr napi-buckets(%d)",
		  napid->napi_mode,
		  hif_napi_cpu_blacklist(napid, BLACKLIST_QUERY),
		  QCA_NAPI_NUM_BUCKETS);

	for (i = 0; i < CE_COUNT_MAX; i++)
		if (napid->ce_map & (0x01 << i)) {
			napii = napid->napis[i];
			if (!napii)
				continue;

			for (j = 0; j < num_possible_cpus(); j++) {
				napis = &(napii->stats[j]);
				n = 0;
				max = sizeof(buf);
				for (k = 0; k < QCA_NAPI_NUM_BUCKETS; k++) {
					n += scnprintf(
						buf + n, max - n,
						" %d",
						napis->napi_budget_uses[k]);
				}

				if (napis->napi_schedules != 0)
					qdf_print("NAPI[%2d]CPU[%d]: %7d %7d %7d %7d %5d %5d %5d %s",
						  i, j,
						  napis->napi_schedules,
						  napis->napi_polls,
						  napis->napi_completes,
						  napis->napi_workdone,
						  napis->time_limit_reached,
						  napis->rxpkt_thresh_reached,
						  napis->cpu_corrected,
						  buf);
			}
		}

	hif_napi_stats(napid);
	return 0;
}


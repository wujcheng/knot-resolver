/*  Copyright (C) 2014-2017 CZ.NIC, z.s.p.o. <knot-dns@labs.nic.cz>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <assert.h>
#include <errno.h>
#include <sys/time.h>
#include <stdio.h>
#include <string.h>

#include <contrib/cleanup.h>
#include <libknot/packet/wire.h>
#include <libknot/rrtype/rdname.h>
#include <libknot/rrtype/rrsig.h>
#include <libdnssec/error.h>

#include "lib/dnssec/nsec.h"
#include "lib/dnssec/nsec3.h"
#include "lib/dnssec.h"
#include "lib/layer.h"
#include "lib/resolve.h"
#include "lib/rplan.h"
#include "lib/utils.h"
#include "lib/defines.h"
#include "lib/module.h"

#define VERBOSE_MSG(qry, fmt...) QRVERBOSE(qry, "vldr", fmt)

#define MAX_REVALIDATION_CNT 2

/**
 * Search in section for given type.
 * @param sec  Packet section.
 * @param type Type to search for.
 * @return     True if found.
 */
static bool section_has_type(const knot_pktsection_t *sec, uint16_t type)
{
	if (!sec) {
		return false;
	}

	for (unsigned i = 0; i < sec->count; ++i) {
		const knot_rrset_t *rr = knot_pkt_rr(sec, i);
		if (rr->type == type) {
			return true;
		}
	}

	return false;
}

static bool pkt_has_type(const knot_pkt_t *pkt, uint16_t type)
{
	if (!pkt) {
		return false;
	}

	if (section_has_type(knot_pkt_section(pkt, KNOT_ANSWER), type)) {
		return true;
	}
	if (section_has_type(knot_pkt_section(pkt, KNOT_AUTHORITY), type)) {
		return true;
	}
	return section_has_type(knot_pkt_section(pkt, KNOT_ADDITIONAL), type);
}

static int validate_section(kr_rrset_validation_ctx_t *vctx, const struct kr_query *qry,
			    knot_mm_t *pool)
{
	if (!vctx) {
		return kr_error(EINVAL);
	}

	/* Can't use qry->zone_cut.name directly, as this name can
	 * change when updating cut information before validation.
	 */
	vctx->zone_name = vctx->keys ? vctx->keys->owner : NULL;

	int validation_result = 0;
	for (ssize_t i = 0; i < vctx->rrs->len; ++i) {
		ranked_rr_array_entry_t *entry = vctx->rrs->at[i];
		const knot_rrset_t *rr = entry->rr;

		if (entry->yielded || vctx->qry_uid != entry->qry_uid) {
			continue;
		}

		if (kr_rank_test(entry->rank, KR_RANK_OMIT)
		    || kr_rank_test(entry->rank, KR_RANK_SECURE)) {
			continue; /* these are already OK */
		}

		if (rr->type == KNOT_RRTYPE_RRSIG) {
			const knot_dname_t *signer_name = knot_rrsig_signer_name(rr->rrs.rdata);
			if (!knot_dname_is_equal(vctx->zone_name, signer_name)) {
				kr_rank_set(&entry->rank, KR_RANK_MISMATCH);
				vctx->err_cnt += 1;
				break;
			}
			kr_rank_set(&entry->rank, KR_RANK_OMIT);
			continue;
		}

		uint8_t rank_orig = entry->rank;
		validation_result = kr_rrset_validate(vctx, rr);
		if (validation_result == kr_ok()) {
			kr_rank_set(&entry->rank, KR_RANK_SECURE);

		} else if (kr_rank_test(rank_orig, KR_RANK_TRY)) {
			WITH_VERBOSE(qry) {
				auto_free char *name_text = kr_dname_text(rr->owner);
				auto_free char *type_text = kr_rrtype_text(rr->type);
				VERBOSE_MSG(qry, ">< failed to validate but skipping: %s %s\n", name_text, type_text);
			}
			vctx->result = kr_ok();
			kr_rank_set(&entry->rank, KR_RANK_TRY);
			/* ^^ BOGUS would be more accurate, but it might change
			 * to MISMATCH on revalidation, e.g. in test val_referral_nods :-/
			 */

		} else if (validation_result == kr_error(ENOENT)) {
			/* no RRSIGs found */
			kr_rank_set(&entry->rank, KR_RANK_MISSING);
			vctx->err_cnt += 1;

		} else {
			kr_rank_set(&entry->rank, KR_RANK_BOGUS);
			vctx->err_cnt += 1;
		}
	}
	return kr_ok();
}

static int validate_records(struct kr_request *req, knot_pkt_t *answer, knot_mm_t *pool, bool has_nsec3)
{
	struct kr_query *qry = req->current_query;
	if (!qry->zone_cut.key) {
		VERBOSE_MSG(qry, "<= no DNSKEY, can't validate\n");
		return kr_error(EBADMSG);
	}

	kr_rrset_validation_ctx_t vctx = {
		.pkt		= answer,
		.rrs		= &req->answ_selected,
		.section_id	= KNOT_ANSWER,
		.keys		= qry->zone_cut.key,
		.zone_name	= qry->zone_cut.name,
		.timestamp	= qry->timestamp.tv_sec,
		.qry_uid	= qry->uid,
		.has_nsec3	= has_nsec3,
		.flags		= 0,
		.err_cnt	= 0,
		.result		= 0
	};

	int ret = validate_section(&vctx, qry, pool);
	req->answ_validated = (vctx.err_cnt == 0);
	if (ret != kr_ok()) {
		return ret;
	}

	uint32_t an_flags = vctx.flags;
	vctx.rrs	  = &req->auth_selected;
	vctx.section_id   = KNOT_AUTHORITY;
	vctx.flags	  = 0;
	vctx.err_cnt	  = 0;
	vctx.result	  = 0;

	ret = validate_section(&vctx, qry, pool);
	req->auth_validated = (vctx.err_cnt == 0);
	if (ret != kr_ok()) {
		return ret;
	}

	/* Records were validated.
	 * If there is wildcard expansion in answer,
	 * or optout - flag the query.
         */
	if (an_flags & KR_DNSSEC_VFLG_WEXPAND) {
		qry->flags.DNSSEC_WEXPAND = true;
	}
	if (an_flags & KR_DNSSEC_VFLG_OPTOUT) {
		qry->flags.DNSSEC_OPTOUT = true;
	}

	return ret;
}

static int validate_keyset(struct kr_request *req, knot_pkt_t *answer, bool has_nsec3)
{
	/* Merge DNSKEY records from answer that are below/at current cut. */
	struct kr_query *qry = req->current_query;
	bool updated_key = false;
	const knot_pktsection_t *an = knot_pkt_section(answer, KNOT_ANSWER);
	for (unsigned i = 0; i < an->count; ++i) {
		const knot_rrset_t *rr = knot_pkt_rr(an, i);
		if (rr->type != KNOT_RRTYPE_DNSKEY
		    || knot_dname_in_bailiwick(rr->owner, qry->zone_cut.name) < 0) {
			continue;
		}
		/* Merge with zone cut (or replace ancestor key). */
		if (!qry->zone_cut.key || !knot_dname_is_equal(qry->zone_cut.key->owner, rr->owner)) {
			qry->zone_cut.key = knot_rrset_copy(rr, qry->zone_cut.pool);
			if (!qry->zone_cut.key) {
				return kr_error(ENOMEM);
			}
			updated_key = true;
		} else {
			int ret = knot_rdataset_merge(&qry->zone_cut.key->rrs,
			                              &rr->rrs, qry->zone_cut.pool);
			if (ret != 0) {
				knot_rrset_free(qry->zone_cut.key, qry->zone_cut.pool);
				qry->zone_cut.key = NULL;
				return ret;
			}
			updated_key = true;
		}
	}

	/* Check if there's a key for current TA. */
	if (updated_key && !(qry->flags.CACHED)) {

		kr_rrset_validation_ctx_t vctx = {
			.pkt		= answer,
			.rrs		= &req->answ_selected,
			.section_id	= KNOT_ANSWER,
			.keys		= qry->zone_cut.key,
			.zone_name	= qry->zone_cut.name,
			.timestamp	= qry->timestamp.tv_sec,
			.qry_uid	= qry->uid,
			.has_nsec3	= has_nsec3,
			.flags		= 0,
			.result		= 0
		};
		int ret = kr_dnskeys_trusted(&vctx, qry->zone_cut.trust_anchor);
		if (ret != 0) {
			knot_rrset_free(qry->zone_cut.key, qry->zone_cut.pool);
			qry->zone_cut.key = NULL;
			return ret;
		}

		if (vctx.flags & KR_DNSSEC_VFLG_WEXPAND) {
			qry->flags.DNSSEC_WEXPAND = true;
		}
		if (vctx.flags & KR_DNSSEC_VFLG_OPTOUT) {
			qry->flags.DNSSEC_OPTOUT = true;
		}

	}
	return kr_ok();
}

static knot_rrset_t *update_ds(struct kr_zonecut *cut, const knot_pktsection_t *sec)
{
	/* Aggregate DS records (if using multiple keys) */
	knot_rrset_t *new_ds = NULL;
	for (unsigned i = 0; i < sec->count; ++i) {
		const knot_rrset_t *rr = knot_pkt_rr(sec, i);
		if (rr->type != KNOT_RRTYPE_DS) {
			continue;
		}
		int ret = 0;
		if (new_ds) {
			ret = knot_rdataset_merge(&new_ds->rrs, &rr->rrs, cut->pool);
		} else {
			new_ds = knot_rrset_copy(rr, cut->pool);
			if (!new_ds) {
				return NULL;
			}
		}
		if (ret != 0) {
			knot_rrset_free(new_ds, cut->pool);
			return NULL;
		}
	}
	return new_ds;	
}

static void mark_insecure_parents(const struct kr_query *qry)
{
	/* If there is a chain of DS queries mark all of them,
	 * then mark first non-DS parent.
	 * Stop if parent is waiting for ns address.
	 * NS can be located at unsigned zone, but still will return
	 * valid DNSSEC records for initial query. */
	struct kr_query *parent = qry->parent;
	while (parent && !parent->flags.AWAIT_IPV4 && !parent->flags.AWAIT_IPV6) {
		parent->flags.DNSSEC_WANT = false;
		parent->flags.DNSSEC_INSECURE = true;
		if (parent->stype != KNOT_RRTYPE_DS &&
		    parent->stype != KNOT_RRTYPE_RRSIG) {
			break;
		}
		parent = parent->parent;
	}
}

static int update_parent_keys(struct kr_request *req, uint16_t answer_type)
{
	struct kr_query *qry = req->current_query;
	struct kr_query *parent = qry->parent;
	assert(parent);
	switch(answer_type) {
	case KNOT_RRTYPE_DNSKEY:
		VERBOSE_MSG(qry, "<= parent: updating DNSKEY\n");
		parent->zone_cut.key = knot_rrset_copy(qry->zone_cut.key, parent->zone_cut.pool);
		if (!parent->zone_cut.key) {
			return KR_STATE_FAIL;
		}
		break;
	case KNOT_RRTYPE_DS:
		VERBOSE_MSG(qry, "<= parent: updating DS\n");
		if (qry->flags.DNSSEC_INSECURE) { /* DS non-existence proven. */
			mark_insecure_parents(qry);
		} else if (qry->flags.DNSSEC_NODS && !qry->flags.FORWARD) {
			if (qry->flags.DNSSEC_OPTOUT) {
				mark_insecure_parents(qry);
			} else {
				int ret = kr_dnssec_matches_name_and_type(&req->auth_selected, qry->uid,
									  qry->sname, KNOT_RRTYPE_NS);
				if (ret == kr_ok()) {
					mark_insecure_parents(qry);
				}
			}
		} else if (qry->flags.DNSSEC_NODS && qry->flags.FORWARD) {
			int ret = kr_dnssec_matches_name_and_type(&req->auth_selected, qry->uid,
								  qry->sname, KNOT_RRTYPE_NS);
			if (ret == kr_ok()) {
				mark_insecure_parents(qry);
			}
		} else { /* DS existence proven. */
			parent->zone_cut.trust_anchor = knot_rrset_copy(qry->zone_cut.trust_anchor, parent->zone_cut.pool);
			if (!parent->zone_cut.trust_anchor) {
				return KR_STATE_FAIL;
			}
		}
		break;
	default: break;
	}
	return kr_ok();
}

static int update_delegation(struct kr_request *req, struct kr_query *qry, knot_pkt_t *answer, bool has_nsec3)
{
	struct kr_zonecut *cut = &qry->zone_cut;

	/* RFC4035 3.1.4. authoritative must send either DS or proof of non-existence.
	 * If it contains neither, resolver must query the parent for the DS (RFC4035 5.2.).
	 * If DS exists, the referral is OK,
	 * otherwise referral is bogus (or an attempted downgrade attack).
	 */


	unsigned section = KNOT_ANSWER;
	const bool referral = !knot_wire_get_aa(answer->wire);
	if (referral) {
		section = KNOT_AUTHORITY;
	} else if (knot_pkt_qtype(answer) == KNOT_RRTYPE_DS &&
		   !(qry->flags.CNAME) &&
		   (knot_wire_get_rcode(answer->wire) != KNOT_RCODE_NXDOMAIN)) {
		section = KNOT_ANSWER;
	} else { /* N/A */
		return kr_ok();
	}

	int ret = 0;
	const knot_dname_t *proved_name = knot_pkt_qname(answer);
	/* Aggregate DS records (if using multiple keys) */
	knot_rrset_t *new_ds = update_ds(cut, knot_pkt_section(answer, section));
	if (!new_ds) {
		/* No DS provided, check for proof of non-existence. */
		if (!has_nsec3) {
			if (referral) {
				/* Check if it is referral to unsigned, rfc4035 5.2 */
				ret = kr_nsec_ref_to_unsigned(answer);
			} else {
				/* No-data answer */
				ret = kr_nsec_existence_denial(answer, KNOT_AUTHORITY, proved_name, KNOT_RRTYPE_DS);
			}
		} else {
			if (referral) {
				/* Check if it is referral to unsigned, rfc5155 8.9 */
				ret = kr_nsec3_ref_to_unsigned(answer);
			} else {
				/* No-data answer, QTYPE is DS, rfc5155 8.6 */
				ret = kr_nsec3_no_data(answer, KNOT_AUTHORITY, proved_name, KNOT_RRTYPE_DS);
			}
			if (ret == kr_error(KNOT_ERANGE)) {
				/* Not bogus, going insecure due to optout */
				ret = 0;
			}
		}

		if (referral && qry->stype != KNOT_RRTYPE_DS &&
		    ret == DNSSEC_NOT_FOUND) {
			/* referral,
			 * qtype is not KNOT_RRTYPE_DS, NSEC\NSEC3 were not found.
			 * Check if DS already was fetched. */
			knot_rrset_t *ta = cut->trust_anchor;
			if (knot_dname_is_equal(cut->name, ta->owner)) {
				/* DS is OK */
				ret = 0;
			}
		} else if (ret != 0) {
			VERBOSE_MSG(qry, "<= bogus proof of DS non-existence\n");
			qry->flags.DNSSEC_BOGUS = true;
		} else if (proved_name[0] != '\0') { /* don't go to insecure for . DS */
			VERBOSE_MSG(qry, "<= DS doesn't exist, going insecure\n");
			qry->flags.DNSSEC_NODS = true;
			/* Rank the corresponding nonauth NS as insecure. */
			for (int i = 0; i < req->auth_selected.len; ++i) {
				ranked_rr_array_entry_t *ns = req->auth_selected.at[i];
				if (ns->qry_uid != qry->uid
				    || !ns->rr
				    || ns->rr->type != KNOT_RRTYPE_NS) {
					continue;
				}
				if (!referral && !knot_dname_is_equal(qry->sname, ns->rr->owner)) {
					continue;
				}
				/* Found the record.  Note: this is slightly fragile
				 * in case there were more NS records in the packet.
				 * As it is now for referrals, kr_nsec*_ref_to_unsigned consider
				 * (only) the first NS record in the packet. */
				if (!kr_rank_test(ns->rank, KR_RANK_AUTH)) { /* sanity */
					ns->rank = KR_RANK_INSECURE;
				}
				break;
			}
		}
		return ret;
	} else if (qry->flags.FORWARD && qry->parent) {
		struct kr_query *parent = qry->parent;
		parent->zone_cut.name = knot_dname_copy(qry->sname, parent->zone_cut.pool);
	}

	/* Extend trust anchor */
	VERBOSE_MSG(qry, "<= DS: OK\n");
	cut->trust_anchor = new_ds;
	return ret;
}

static const knot_dname_t *find_first_signer(ranked_rr_array_t *arr)
{
	for (size_t i = 0; i < arr->len; ++i) {
		ranked_rr_array_entry_t *entry = arr->at[i];
		const knot_rrset_t *rr = entry->rr;
		if (entry->yielded ||
		    (!kr_rank_test(entry->rank, KR_RANK_INITIAL) &&
		     !kr_rank_test(entry->rank, KR_RANK_TRY) &&
		     !kr_rank_test(entry->rank, KR_RANK_MISMATCH))) {
			continue;
		}
		if (rr->type == KNOT_RRTYPE_RRSIG) {
			return knot_rrsig_signer_name(rr->rrs.rdata);
		}
	}
	return NULL;
}

static const knot_dname_t *signature_authority(struct kr_request *req)
{
	const knot_dname_t *signer_name = find_first_signer(&req->answ_selected);
	if (!signer_name) {
		signer_name = find_first_signer(&req->auth_selected);
	}
	return signer_name;
}

static int rrsig_not_found(kr_layer_t *ctx, const knot_rrset_t *rr)
{
	struct kr_request *req = ctx->req;
	struct kr_query *qry = req->current_query;

	/* Parent-side record, so don't ask for RRSIG.
	 * We won't receive it anyway. */
	if (qry->stype == KNOT_RRTYPE_DS) {
		return KR_STATE_FAIL;
	}

	struct kr_zonecut *cut = &qry->zone_cut;
	const knot_dname_t *cut_name_start = qry->zone_cut.name;
	bool use_cut = true;
	if (knot_dname_in_bailiwick(rr->owner, cut_name_start) < 0) {
		int zone_labels = knot_dname_labels(qry->zone_cut.name, NULL);
		int matched_labels = knot_dname_matched_labels(qry->zone_cut.name, rr->owner);
		int skip_labels = zone_labels - matched_labels;
		while (skip_labels--) {
			cut_name_start = knot_wire_next_label(cut_name_start, NULL);
		}
		/* try to find the name wanted among ancestors */
		use_cut = false;
		while (cut->parent) {
			cut = cut->parent;
			if (knot_dname_is_equal(cut_name_start, cut->name)) {
				use_cut = true;
				break;
			}
		};
	}
	struct kr_rplan *rplan = &req->rplan;
	struct kr_query *next = kr_rplan_push(rplan, qry, rr->owner, rr->rclass, KNOT_RRTYPE_RRSIG);
	if (!next) {
		return KR_STATE_FAIL;
	}
	kr_zonecut_init(&next->zone_cut, cut_name_start, &req->pool);
	if (use_cut) {
		kr_zonecut_copy(&next->zone_cut, cut);
		kr_zonecut_copy_trust(&next->zone_cut, cut);
	} else {
		next->flags.AWAIT_CUT = true;
	}
	if (qry->flags.FORWARD) {
		next->flags.AWAIT_CUT = false;
	}
	next->flags.DNSSEC_WANT = true;
	return KR_STATE_YIELD;
}

static int check_validation_result(kr_layer_t *ctx, ranked_rr_array_t *arr)
{
	int ret = KR_STATE_DONE;
	struct kr_request *req = ctx->req;
	struct kr_query *qry = req->current_query;
	ranked_rr_array_entry_t *invalid_entry = NULL;
	for (size_t i = 0; i < arr->len; ++i) {
		ranked_rr_array_entry_t *entry = arr->at[i];
		if (entry->yielded || entry->qry_uid != qry->uid) {
			continue;
		}
		if (kr_rank_test(entry->rank, KR_RANK_MISMATCH)) {
			invalid_entry = entry;
			break;
		} else if (kr_rank_test(entry->rank, KR_RANK_MISSING) &&
			   !invalid_entry) {
			invalid_entry = entry;
		} else if (kr_rank_test(entry->rank, KR_RANK_OMIT)) {
			continue;
		} else if (!kr_rank_test(entry->rank, KR_RANK_SECURE) &&
			   !invalid_entry) {
			invalid_entry = entry;
		}
	}

	if (!invalid_entry) {
		return ret;
	}

	if (!kr_rank_test(invalid_entry->rank, KR_RANK_SECURE) &&
	    (++(invalid_entry->revalidation_cnt) > MAX_REVALIDATION_CNT)) {
		VERBOSE_MSG(qry, "<= continuous revalidation, fails\n");
		qry->flags.DNSSEC_BOGUS = true;
		return KR_STATE_FAIL;
	}

	const knot_rrset_t *rr = invalid_entry->rr;
	if (kr_rank_test(invalid_entry->rank, KR_RANK_MISMATCH)) {
		const knot_dname_t *signer_name = knot_rrsig_signer_name(rr->rrs.rdata);
		if (knot_dname_in_bailiwick(signer_name, qry->zone_cut.name) > 0) {
			qry->zone_cut.name = knot_dname_copy(signer_name, &req->pool);
			qry->flags.AWAIT_CUT = true;
		} else if (!knot_dname_is_equal(signer_name, qry->zone_cut.name)) {
			if (qry->zone_cut.parent) {
				memcpy(&qry->zone_cut, qry->zone_cut.parent, sizeof(qry->zone_cut));
			} else {
				qry->flags.AWAIT_CUT = true;
			}
			qry->zone_cut.name = knot_dname_copy(signer_name, &req->pool);
		}
		VERBOSE_MSG(qry, ">< cut changed (new signer), needs revalidation\n");
		ret = KR_STATE_YIELD;
	} else if (kr_rank_test(invalid_entry->rank, KR_RANK_MISSING)) {
		WITH_VERBOSE(qry) {
			auto_free char *name_text = kr_dname_text(invalid_entry->rr->owner);
			auto_free char *type_text = kr_rrtype_text(invalid_entry->rr->type);
			VERBOSE_MSG(qry, ">< no valid RRSIGs found for %s %s\n", name_text, type_text);
		}
		ret = rrsig_not_found(ctx, rr);
	} else if (!kr_rank_test(invalid_entry->rank, KR_RANK_SECURE)) {
		qry->flags.DNSSEC_BOGUS = true;
		ret = KR_STATE_FAIL;
	}

	return ret;
}

static bool check_empty_answer(kr_layer_t *ctx, knot_pkt_t *pkt)
{
	struct kr_request *req	= ctx->req;
	struct kr_query *qry	= req->current_query;
	ranked_rr_array_t *arr	= &req->answ_selected;
	size_t num_entries = 0;
	for (size_t i = 0; i < arr->len; ++i) {
		ranked_rr_array_entry_t *entry = arr->at[i];
		const knot_rrset_t *rr = entry->rr;
		if (rr->type == KNOT_RRTYPE_RRSIG && qry->stype != KNOT_RRTYPE_RRSIG) {
			continue;
		}
		if (entry->qry_uid == qry->uid) {
			++num_entries;
		}
	}
	const knot_pktsection_t *an = knot_pkt_section(pkt, KNOT_ANSWER);
	return ((an->count != 0) && (num_entries == 0)) ? false : true;
}

static int unsigned_forward(kr_layer_t *ctx, knot_pkt_t *pkt)
{
	struct kr_request *req = ctx->req;
	struct kr_query *qry = req->current_query;
	const uint16_t qtype = knot_pkt_qtype(pkt);
	const uint8_t pkt_rcode = knot_wire_get_rcode(pkt->wire);
	bool nods = false;
	bool ns_exist = true;
	for (int i = 0; i < req->rplan.resolved.len; ++i) {
		struct kr_query *q = req->rplan.resolved.at[i];
		if (q->sclass == qry->sclass &&
		    q->stype == KNOT_RRTYPE_DS &&
		    knot_dname_is_equal(q->sname, qry->sname)) {
			nods = true;
			if (!(q->flags.DNSSEC_OPTOUT)) {
				int ret = kr_dnssec_matches_name_and_type(&req->auth_selected, q->uid,
									  qry->sname, KNOT_RRTYPE_NS);
				ns_exist = (ret == kr_ok());
			}
		}
	}

	if (nods && ns_exist && qtype == KNOT_RRTYPE_NS) {
		qry->flags.DNSSEC_WANT = false;
		qry->flags.DNSSEC_INSECURE = true;
		if (qry->forward_flags.CNAME) {
			assert(qry->cname_parent);
			qry->cname_parent->flags.DNSSEC_WANT = false;
			qry->cname_parent->flags.DNSSEC_INSECURE = true;
		} else if (pkt_rcode == KNOT_RCODE_NOERROR && qry->parent != NULL) {
			const knot_pktsection_t *sec = knot_pkt_section(pkt, KNOT_ANSWER);
			const knot_rrset_t *rr = knot_pkt_rr(sec, 0);
			if (rr->type == KNOT_RRTYPE_NS) {
				qry->parent->zone_cut.name = knot_dname_copy(rr->owner, &req->pool);
				qry->parent->flags.DNSSEC_WANT = false;
				qry->parent->flags.DNSSEC_INSECURE = true;
			}
		}
		while (qry->parent) {
			qry = qry->parent;
			qry->flags.DNSSEC_WANT = false;
			qry->flags.DNSSEC_INSECURE = true;
			if (qry->forward_flags.CNAME) {
				assert(qry->cname_parent);
				qry->cname_parent->flags.DNSSEC_WANT = false;
				qry->cname_parent->flags.DNSSEC_INSECURE = true;
			}
		}
		return KR_STATE_DONE;
	}

	if (ctx->state == KR_STATE_YIELD) {
		return KR_STATE_DONE;
	}

	if (!nods && qtype != KNOT_RRTYPE_DS) {
		struct kr_rplan *rplan = &req->rplan;
		struct kr_query *next = kr_rplan_push(rplan, qry, qry->sname, qry->sclass, KNOT_RRTYPE_DS);
		if (!next) {
			return KR_STATE_FAIL;
		}
		kr_zonecut_set(&next->zone_cut, qry->zone_cut.name);
		kr_zonecut_copy_trust(&next->zone_cut, &qry->zone_cut);
		next->flags.DNSSEC_WANT = true;
	}

	return KR_STATE_YIELD;
}

static int check_signer(kr_layer_t *ctx, knot_pkt_t *pkt)
{
	struct kr_request *req = ctx->req;
	struct kr_query *qry = req->current_query;
	const knot_dname_t *ta_name = qry->zone_cut.trust_anchor ? qry->zone_cut.trust_anchor->owner : NULL;
	const knot_dname_t *signer = signature_authority(req);
	if (ta_name && (!signer || !knot_dname_is_equal(ta_name, signer))) {
		/* check all newly added RRSIGs */
		if (!signer) {
			if (qry->flags.FORWARD) {
				return unsigned_forward(ctx, pkt);
			}
			/* Not a DNSSEC-signed response. */
			if (ctx->state == KR_STATE_YIELD) {
				/* Already yielded for revalidation.
				 * It means that trust chain is OK and
				 * transition to INSECURE hasn't occurred.
				 * Let the validation logic ask about RRSIG. */
				return KR_STATE_DONE;
			}
			/* Ask parent for DS
			 * to prove transition to INSECURE. */
			const uint16_t qtype = knot_pkt_qtype(pkt);
			const knot_dname_t *qname = knot_pkt_qname(pkt);
			if (qtype == KNOT_RRTYPE_NS &&
			    knot_dname_in_bailiwick(qname, qry->zone_cut.name) > 0) {
				/* Server is authoritative
				 * for both parent and child,
				 * and child zone is not signed. */
				qry->zone_cut.name = knot_dname_copy(qname, &req->pool);
			}
		} else if (knot_dname_in_bailiwick(signer, qry->zone_cut.name) > 0) {
			if (!(qry->flags.FORWARD)) {
				/* Key signer is below current cut, advance and refetch keys. */
				qry->zone_cut.name = knot_dname_copy(signer, &req->pool);
			} else {
				/* Check if DS does not exist. */
				struct kr_query *q = kr_rplan_find_resolved(&req->rplan, NULL,
									    signer, qry->sclass, KNOT_RRTYPE_DS);
				if (q && q->flags.DNSSEC_NODS) {
					qry->flags.DNSSEC_WANT = false;
					qry->flags.DNSSEC_INSECURE = true;
					if (qry->parent) {
						qry->parent->flags.DNSSEC_WANT = false;
						qry->parent->flags.DNSSEC_INSECURE = true;
					}
				} else if (qry->stype != KNOT_RRTYPE_DS) {
					struct kr_rplan *rplan = &req->rplan;
					struct kr_query *next = kr_rplan_push(rplan, qry, qry->sname,
									      qry->sclass, KNOT_RRTYPE_DS);
					if (!next) {
						return KR_STATE_FAIL;
					}
					kr_zonecut_set(&next->zone_cut, qry->zone_cut.name);
					kr_zonecut_copy_trust(&next->zone_cut, &qry->zone_cut);
					next->flags.DNSSEC_WANT = true;
				}
			}
		} else if (!knot_dname_is_equal(signer, qry->zone_cut.name)) {
			/* Key signer is above the current cut, so we can't validate it. This happens when
			   a server is authoritative for both grandparent, parent and child zone.
			   Ascend to parent cut, and refetch authority for signer. */
			if (qry->zone_cut.parent) {
				memcpy(&qry->zone_cut, qry->zone_cut.parent, sizeof(qry->zone_cut));
			} else {
				qry->flags.AWAIT_CUT = true;
			}
			qry->zone_cut.name = knot_dname_copy(signer, &req->pool);
		}

		/* zone cut matches, but DS/DNSKEY doesn't => refetch. */
		VERBOSE_MSG(qry, ">< cut changed, needs revalidation\n");
		if ((qry->flags.FORWARD) && qry->stype != KNOT_RRTYPE_DS) {
			struct kr_rplan *rplan = &req->rplan;
			struct kr_query *next = kr_rplan_push(rplan, qry, signer,
							qry->sclass, KNOT_RRTYPE_DS);
			if (!next) {
				return KR_STATE_FAIL;
			}
			kr_zonecut_set(&next->zone_cut, qry->zone_cut.name);
			kr_zonecut_copy_trust(&next->zone_cut, &qry->zone_cut);
			next->flags.DNSSEC_WANT = true;
			return KR_STATE_YIELD;
		}
		if (!(qry->flags.FORWARD)) {
			return KR_STATE_YIELD;
		}
	}
	return KR_STATE_DONE;
}

/** Change ranks of RRs from this single iteration:
 * _INITIAL or _TRY or _MISSING -> rank_to_set.
 *
 * Optionally do this only in a `bailiwick` (if not NULL).
 * Iterator shouldn't have selected such records, but we check to be sure. */
static void rank_records(kr_layer_t *ctx, enum kr_rank rank_to_set,
			 const knot_dname_t *bailiwick)
{
	struct kr_request *req	   = ctx->req;
	struct kr_query *qry	   = req->current_query;
	ranked_rr_array_t *ptrs[2] = { &req->answ_selected, &req->auth_selected };
	for (size_t i = 0; i < 2; ++i) {
		ranked_rr_array_t *arr = ptrs[i];
		for (size_t j = 0; j < arr->len; ++j) {
			ranked_rr_array_entry_t *entry = arr->at[j];
			if (entry->qry_uid != qry->uid) {
				continue;
			}
			if (bailiwick && knot_dname_in_bailiwick(entry->rr->owner,
								 bailiwick) < 0) {
				continue;
			}
			if (kr_rank_test(entry->rank, KR_RANK_INITIAL)
			    || kr_rank_test(entry->rank, KR_RANK_TRY)
			    || kr_rank_test(entry->rank, KR_RANK_MISSING)) {
				kr_rank_set(&entry->rank, rank_to_set);
			}
		}
	}
}

static void check_wildcard(kr_layer_t *ctx)
{
	struct kr_request *req	   = ctx->req;
	struct kr_query *qry	   = req->current_query;
	ranked_rr_array_t *ptrs[2] = { &req->answ_selected, &req->auth_selected };

	for (int i = 0; i < 2; ++i) {
		ranked_rr_array_t *arr = ptrs[i];
		for (ssize_t j = 0; j < arr->len; ++j) {
			ranked_rr_array_entry_t *entry = arr->at[j];
			const knot_rrset_t *rrsigs = entry->rr;

			if (qry->uid != entry->qry_uid) {
				continue;
			}

			if (rrsigs->type != KNOT_RRTYPE_RRSIG) {
				continue;
			}

			int owner_labels = knot_dname_labels(rrsigs->owner, NULL);

			knot_rdata_t *rdata_k = rrsigs->rrs.rdata;
			for (int k = 0; k < rrsigs->rrs.count;
					++k, rdata_k = knot_rdataset_next(rdata_k)) {
				if (knot_rrsig_labels(rdata_k) != owner_labels) {
					qry->flags.DNSSEC_WEXPAND = true;
				}
			}
		}
	}
}

/** Just for wildcard_adjust_to_wire() */
static bool rr_is_for_wildcard(const ranked_rr_array_entry_t *entry)
{
	switch (kr_rrset_type_maysig(entry->rr)) {
	case KNOT_RRTYPE_NSEC:
	case KNOT_RRTYPE_NSEC3:
		return true;
	default:
		return false;
	}
}
/** In case of wildcard expansion, mark required authority RRs by to_wire. */
static int wildcard_adjust_to_wire(struct kr_request *req, const struct kr_query *qry)
{
	if (!qry->parent && qry->flags.DNSSEC_WEXPAND) {
		return kr_ranked_rrarray_set_wire(&req->auth_selected, true,
				qry->uid, true, &rr_is_for_wildcard);
	}
	return kr_ok();
}

static int validate(kr_layer_t *ctx, knot_pkt_t *pkt)
{
	int ret = 0;
	struct kr_request *req = ctx->req;
	struct kr_query *qry = req->current_query;

	/* Ignore faulty or unprocessed responses. */
	if (ctx->state & (KR_STATE_FAIL|KR_STATE_CONSUME)) {
		return ctx->state;
	}

	/* Pass-through if user doesn't want secure answer or stub. */
	/* @todo: Validating stub resolver mode. */
	if (qry->flags.STUB) {
		rank_records(ctx, KR_RANK_OMIT, NULL);
		return ctx->state;
	}
	uint8_t pkt_rcode = knot_wire_get_rcode(pkt->wire);
	if ((qry->flags.FORWARD) &&
	    pkt_rcode != KNOT_RCODE_NOERROR &&
	    pkt_rcode != KNOT_RCODE_NXDOMAIN) {
		do {
			qry->flags.DNSSEC_BOGUS = true;
			if (qry->cname_parent) {
				qry->cname_parent->flags.DNSSEC_BOGUS = true;
			}
			qry = qry->parent;
		} while (qry);
		ctx->state = KR_STATE_DONE;
		return ctx->state;
	}

	if (!(qry->flags.DNSSEC_WANT)) {
		const bool is_insec = qry->flags.CACHED && qry->flags.DNSSEC_INSECURE;
		if ((qry->flags.DNSSEC_INSECURE)) {
			rank_records(ctx, KR_RANK_INSECURE, qry->zone_cut.name);
		}
		if (is_insec && qry->parent != NULL) {
			/* We have got insecure answer from cache.
			 * Mark parent(s) as insecure. */
			mark_insecure_parents(qry);
			VERBOSE_MSG(qry, "<= cached insecure response, going insecure\n");
			ctx->state = KR_STATE_DONE;
		} else if (ctx->state == KR_STATE_YIELD) {
			/* Transition to unsecure state
			   occurred during revalidation.
			   if state remains YIELD, answer will not be cached.
			   Let cache layers to work. */
			ctx->state = KR_STATE_DONE;
		}
		return ctx->state;
	}

	/* Pass-through if CD bit is set. */
	if (knot_wire_get_cd(req->answer->wire)) {
		check_wildcard(ctx);
		wildcard_adjust_to_wire(req, qry);
		rank_records(ctx, KR_RANK_OMIT, NULL);
		return ctx->state;
	}
	/* Answer for RRSIG may not set DO=1, but all records MUST still validate. */
	bool use_signatures = (knot_pkt_qtype(pkt) != KNOT_RRTYPE_RRSIG);
	if (!(qry->flags.CACHED) && !knot_pkt_has_dnssec(pkt) && !use_signatures) {
		VERBOSE_MSG(qry, "<= got insecure response\n");
		qry->flags.DNSSEC_BOGUS = true;
		return KR_STATE_FAIL;
	}

	/* Check if this is a DNSKEY answer, check trust chain and store. */
	uint16_t qtype = knot_pkt_qtype(pkt);
	bool has_nsec3 = pkt_has_type(pkt, KNOT_RRTYPE_NSEC3);
	const knot_pktsection_t *an = knot_pkt_section(pkt, KNOT_ANSWER);
	const bool referral = (an->count == 0 && !knot_wire_get_aa(pkt->wire));

	if (!(qry->flags.CACHED) && knot_wire_get_aa(pkt->wire)) {
		/* Check if answer if not empty,
		 * but iterator has not selected any records. */
		if (!check_empty_answer(ctx, pkt)) {
			VERBOSE_MSG(qry, "<= no useful RR in authoritative answer\n");
			qry->flags.DNSSEC_BOGUS = true;
			return KR_STATE_FAIL;
		}
		/* Track difference between current TA and signer name.
		 * This indicates that the NS is auth for both parent-child,
		 * and we must update DS/DNSKEY to validate it.
		 */
		ret = check_signer(ctx, pkt);
		if (ret != KR_STATE_DONE) {
			return ret;
		}
		if (qry->flags.FORWARD && qry->flags.DNSSEC_INSECURE) {
			return KR_STATE_DONE;
		}
	}

	if (knot_wire_get_aa(pkt->wire) && qtype == KNOT_RRTYPE_DNSKEY) {
		ret = validate_keyset(req, pkt, has_nsec3);
		if (ret == kr_error(EAGAIN)) {
			VERBOSE_MSG(qry, ">< cut changed, needs revalidation\n");
			return KR_STATE_YIELD;
		} else if (ret == kr_error(DNSSEC_INVALID_DS_ALGORITHM)) {
			VERBOSE_MSG(qry, ">< all DS entries use unsupported algorithm pairs, going insecure\n");
			/* ^ the message is a bit imprecise to avoid being too verbose */
			qry->flags.DNSSEC_WANT = false;
			qry->flags.DNSSEC_INSECURE = true;
			rank_records(ctx, KR_RANK_INSECURE, qry->zone_cut.name);
			mark_insecure_parents(qry);
			return KR_STATE_DONE;
		} else if (ret != 0) {
			VERBOSE_MSG(qry, "<= bad keys, broken trust chain\n");
			qry->flags.DNSSEC_BOGUS = true;
			return KR_STATE_FAIL;
		}
	}

	/* Validate non-existence proof if not positive answer.
	 * In case of CNAME, iterator scheduled a sibling query for the target,
	 * so we just drop the negative piece of information and don't try to prove it.
	 * TODO: not ideal; with aggressive cache we'll at least avoid the extra packet. */
	if (!qry->flags.CACHED && pkt_rcode == KNOT_RCODE_NXDOMAIN && !qry->flags.CNAME) {
		/* @todo If knot_pkt_qname(pkt) is used instead of qry->sname then the tests crash. */
		if (!has_nsec3) {
			ret = kr_nsec_name_error_response_check(pkt, KNOT_AUTHORITY, qry->sname);
		} else {
			ret = kr_nsec3_name_error_response_check(pkt, KNOT_AUTHORITY, qry->sname);
		}
		if (has_nsec3 && (ret == kr_error(KNOT_ERANGE))) {
			/* NXDOMAIN proof is OK,
			 * but NSEC3 that covers next closer name
			 * (or wildcard at next closer name) has opt-out flag.
			 * RFC5155 9.2; AD flag can not be set */
			qry->flags.DNSSEC_OPTOUT = true;
			VERBOSE_MSG(qry, "<= can't prove NXDOMAIN due to optout, going insecure\n");
		} else if (ret != 0) {
			VERBOSE_MSG(qry, "<= bad NXDOMAIN proof\n");
			qry->flags.DNSSEC_BOGUS = true;
			return KR_STATE_FAIL;
		}
	}

	/* @todo WTH, this needs API that just tries to find a proof and the caller
	 * doesn't have to worry about NSEC/NSEC3
	 * @todo rework this
	 * CNAME: same as the NXDOMAIN case above */
	if (!qry->flags.CACHED && pkt_rcode == KNOT_RCODE_NOERROR && !qry->flags.CNAME) {
		bool no_data = (an->count == 0 && knot_wire_get_aa(pkt->wire));
		if (no_data) {
			/* @todo
			 * ? quick mechanism to determine which check to preform first
			 * ? merge the functionality together to share code/resources
			 */
			if (!has_nsec3) {
				ret = kr_nsec_existence_denial(pkt, KNOT_AUTHORITY, knot_pkt_qname(pkt), knot_pkt_qtype(pkt));
			} else {
				ret = kr_nsec3_no_data(pkt, KNOT_AUTHORITY, knot_pkt_qname(pkt), knot_pkt_qtype(pkt));
			}
			if (ret != 0) {
				if (has_nsec3 && (ret == kr_error(KNOT_ERANGE))) {
					VERBOSE_MSG(qry, "<= can't prove NODATA due to optout, going insecure\n");
					qry->flags.DNSSEC_OPTOUT = true;
					/* Could not return from here,
					 * we must continue, validate NSEC\NSEC3 and
					 * call update_parent_keys() to mark
					 * parent queries as insecured */
				} else {
					VERBOSE_MSG(qry, "<= bad NODATA proof\n");
					qry->flags.DNSSEC_BOGUS = true;
					return KR_STATE_FAIL;
				}
			}
		}
	}

	/* Validate all records, fail as bogus if it doesn't match.
	 * Do not revalidate data from cache, as it's already trusted. */
	if (!(qry->flags.CACHED)) {
		ret = validate_records(req, pkt, req->rplan.pool, has_nsec3);
		if (ret != 0) {
			/* something exceptional - no DNS key, empty pointers etc
			 * normally it shoudn't happen */
			VERBOSE_MSG(qry, "<= couldn't validate RRSIGs\n");
			qry->flags.DNSSEC_BOGUS = true;
			return KR_STATE_FAIL;
		}
		/* check validation state and spawn subrequests */
		if (!req->answ_validated) {
			ret = check_validation_result(ctx, &req->answ_selected);
			if (ret != KR_STATE_DONE) {
				return ret;
			}
		}
		if (!req->auth_validated) {
			ret = check_validation_result(ctx, &req->auth_selected);
			if (ret != KR_STATE_DONE) {
				return ret;
			}
		}
	}

	wildcard_adjust_to_wire(req, qry);

	/* Check and update current delegation point security status. */
	ret = update_delegation(req, qry, pkt, has_nsec3);
	if (ret == DNSSEC_NOT_FOUND && qry->stype != KNOT_RRTYPE_DS) {
		if (ctx->state == KR_STATE_YIELD) {
			VERBOSE_MSG(qry, "<= can't validate referral\n");
			qry->flags.DNSSEC_BOGUS = true;
			return KR_STATE_FAIL;
		} else {
			/* Check the trust chain and query DS\DNSKEY if needed. */
			VERBOSE_MSG(qry, "<= DS\\NSEC was not found, querying for DS\n");
			return KR_STATE_YIELD;
		}
	} else if (ret != 0) {
		return KR_STATE_FAIL;
	} else if (pkt_rcode == KNOT_RCODE_NOERROR &&
		   referral &&
		   ((!qry->flags.DNSSEC_WANT && qry->flags.DNSSEC_INSECURE) ||
		   (qry->flags.DNSSEC_NODS))) {
		/* referral with proven DS non-existance */
		qtype = KNOT_RRTYPE_DS;
	}
	/* Update parent query zone cut */
	if (qry->parent) {
		if (update_parent_keys(req, qtype) != 0) {
			return KR_STATE_FAIL;
		}
	}

	if (qry->flags.FORWARD && qry->parent) {
		if (pkt_rcode == KNOT_RCODE_NXDOMAIN) {
			qry->parent->forward_flags.NO_MINIMIZE = true;
		}
	}
	VERBOSE_MSG(qry, "<= answer valid, OK\n");
	return KR_STATE_DONE;
}
/** Module implementation. */
const kr_layer_api_t *validate_layer(struct kr_module *module)
{
	static const kr_layer_api_t _layer = {
		.consume = &validate,
	};
	/* Store module reference */
	return &_layer;
}

int validate_init(struct kr_module *module)
{
	return kr_ok();
}

KR_MODULE_EXPORT(validate)

#undef VERBOSE_MSG

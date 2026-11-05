/* custom-fw/test/test_usb_desc.c - the USB descriptor tables (include/pa_usb_desc.h) checked structurally, on the PC.
 * A host that refuses a descriptor gives no reason, so every length, interface number, endpoint and cross-reference is verified here: the configuration is walked byte by byte
 * against the USB 2.0, USB Audio 1.0 and USB MIDI 1.0 rules, and the audio format is compared with the one read from the stock firmware image.
 * Output: one line per check, "COMMS-TEST|usb_desc|name|PASS/FAIL|0|detail", and a COMMS-SUITE line. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "pa_usb_desc.h"

static int g_pass, g_fail;
static void check(const char *name, int ok, const char *fmt, ...) {
    (void)fmt;
    printf("COMMS-TEST|usb_desc|%s|%s|0|%s\n", name, ok ? "PASS" : "FAIL", ok ? "ok" : fmt);
    if (ok) g_pass++; else g_fail++;
}
#define CHECK(name, cond, msg) check(name, (cond) ? 1 : 0, msg)

/* the stock's audio format descriptor (stock app.bin 0x62851 and 0x62885, both): format type I, 2 channels, 3 bytes, 24 bit, one rate, 44,100 Hz */
static const uint8_t STOCK_FORMAT[14] = { 0x0E, 0x24, 0x02, 0x01, 0x02, 0x03, 0x18, 0x02, 0x44, 0xAC, 0x00, 0x80, 0xBB, 0x00 };

struct itf { int seen, alts, cls, sub, eps; };

int main(void) {
    const uint8_t *c = PA_CFG_DESC;
    const unsigned total = (unsigned)sizeof PA_CFG_DESC;

    /* ---- the device descriptor ---- */
    CHECK("device_descriptor_shape", PA_DEV_DESC[0] == 18 && PA_DEV_DESC[1] == 1 && PA_DEV_DESC[17] == 1, "length / type / configuration count");
    CHECK("device_class_is_iad", PA_DEV_DESC[4] == 0xEF && PA_DEV_DESC[5] == 0x02 && PA_DEV_DESC[6] == 0x01, "a device with interface associations must be class EF/02/01");
    CHECK("device_bcd_marks_the_new_layout", (PA_DEV_DESC[12] | PA_DEV_DESC[13] << 8) == PA_USB_DESC_VERSION, "bcdDevice");

    /* ---- the configuration header ---- */
    CHECK("config_total_length_matches_the_table", (c[2] | c[3] << 8) == (int)total, "wTotalLength != sizeof");
    CHECK("config_header_shape", c[0] == 9 && c[1] == 2, "bLength / type");

    /* ---- walk every descriptor ---- */
    unsigned p = 0, last_nonzero = 0;
    struct itf itf[8]; memset(itf, 0, sizeof itf);
    int cur = -1, n_itf_decl = c[4];
    int ep_addr[16], ep_attr[16], ep_maxp[16], ep_itf[16], n_ep = 0;
    int iad_first[4], iad_cnt[4], n_iad = 0;
    int term_id[8], term_type[8], term_dir[8], n_term = 0;        /* dir: 1 input, 2 output */
    int ac_hdr_total[4], ac_hdr_calc[4], ac_n = 0, ac_itf_of[4];
    int ac_list[4][8], ac_list_n[4];
    int as_link[8], as_itf[8], n_as = 0;
    int fmt_ok = 0, fmt_n = 0;
    int ms_total = -1, ms_calc = 0, ms_active = 0;
    int bad_len = 0, overrun = 0;
    memset(ac_hdr_calc, 0, sizeof ac_hdr_calc);
    int in_ac = -1;
    while (p < total) {
        unsigned L = c[p], T = c[p + 1];
        if (L < 2 || p + L > total) { bad_len++; break; }
        last_nonzero = p + L;
        if (T == 11) { iad_first[n_iad] = c[p + 2]; iad_cnt[n_iad] = c[p + 3]; n_iad++; if (L != 8) bad_len++; }
        else if (T == 4) {
            int num = c[p + 2], alt = c[p + 3];
            if (num >= 8) { overrun++; break; }
            if (alt == 0) { itf[num].seen = 1; itf[num].cls = c[p + 5]; itf[num].sub = c[p + 6]; }
            itf[num].alts++; itf[num].eps = c[p + 4]; cur = num;
            in_ac = (c[p + 5] == 1 && c[p + 6] == 1) ? cur : -1;
            ms_active = 0;
            if (L != 9) bad_len++;
            if (c[p + 5] == 1 && c[p + 6] == 3) ms_active = 1;
        } else if (T == 5) {
            ep_addr[n_ep] = c[p + 2]; ep_attr[n_ep] = c[p + 3]; ep_maxp[n_ep] = c[p + 4] | c[p + 5] << 8; ep_itf[n_ep] = cur; n_ep++;
            if (ms_active) ms_calc += (int)L;
            if (itf[cur].cls == 1 && itf[cur].sub == 2) { if (L != 9) bad_len++; }      /* an audio endpoint carries bRefresh and bSynchAddress */
        } else if (T == 0x24) {
            unsigned S = c[p + 2];
            if (in_ac >= 0) {                                       /* audio-control class-specific */
                if (S != 1 && ac_n > 0) ac_hdr_calc[ac_n - 1] += (int)L;      /* units and terminals add to the block opened by the last header */
                if (S == 1) {
                    ac_hdr_total[ac_n] = c[p + 5] | c[p + 6] << 8; ac_itf_of[ac_n] = in_ac; ac_hdr_calc[ac_n] = (int)L;
                    ac_list_n[ac_n] = c[p + 7];
                    if ((int)L != 8 + c[p + 7]) bad_len++;
                    for (int i = 0; i < c[p + 7] && i < 8; i++) ac_list[ac_n][i] = c[p + 8 + i];
                    ac_n++;
                } else if (S == 2) { term_id[n_term] = c[p + 3]; term_type[n_term] = c[p + 4] | c[p + 5] << 8; term_dir[n_term] = 1; n_term++; if (L != 12) bad_len++; }
                else if (S == 3) { term_id[n_term] = c[p + 3]; term_type[n_term] = c[p + 4] | c[p + 5] << 8; term_dir[n_term] = 2; n_term++; if (L != 9) bad_len++; }
            } else if (itf[cur].cls == 1 && itf[cur].sub == 2) {    /* audio streaming class-specific */
                if (S == 1) { as_link[n_as] = c[p + 3]; as_itf[n_as] = cur; n_as++; if (L != 7) bad_len++; }
                else if (S == 2) { fmt_n++; if (L == sizeof STOCK_FORMAT && !memcmp(c + p, STOCK_FORMAT, sizeof STOCK_FORMAT)) fmt_ok++; }
            } else if (ms_active) {
                if (S == 1) ms_total = c[p + 5] | c[p + 6] << 8;
                ms_calc += (int)L;
            }
        } else if (T == 0x25) {
            if (ms_active) ms_calc += (int)L;
        }
        p += L;
    }
    CHECK("walk_consumes_the_whole_table_exactly", p == total && !bad_len && !overrun, "the descriptors do not add up to wTotalLength, or one has a bad bLength");
    CHECK("no_zero_padding_at_the_end", last_nonzero == total, "the array is larger than its initializer (trailing zeros)");

    /* ---- interfaces ---- */
    int n_itf = 0; for (int i = 0; i < 8; i++) if (itf[i].seen) n_itf++;
    CHECK("interface_count_declared", n_itf == n_itf_decl, "bNumInterfaces differs from the interfaces present");
    int seq = 1; for (int i = 0; i < n_itf; i++) if (!itf[i].seen) seq = 0;
    CHECK("interfaces_numbered_from_zero_without_gaps", seq, "gap in the interface numbers");
    CHECK("audio_streaming_interfaces_have_two_alts_midi_one", itf[1].alts == 2 && itf[2].alts == 2 && itf[3].alts == 1 && itf[4].alts == 1, "alternate settings");
    {
        /* alt 0 of interfaces 1 and 2 must carry zero endpoints: walk again for the bNumEndpoints of each alt 0 */
        int ok = 1; unsigned q = 0;
        while (q + 1 < total && c[q] >= 2) { if (c[q + 1] == 4 && c[q + 3] == 0 && (c[q + 2] == 1 || c[q + 2] == 2) && c[q + 4] != 0) ok = 0; q += c[q]; }
        CHECK("audio_streaming_alt0_is_zero_bandwidth", ok, "alt 0 of an audio streaming interface has an endpoint");
    }

    /* ---- interface associations ---- */
    CHECK("two_interface_associations", n_iad == 2, "expected the audio and the MIDI function");
    CHECK("iad_audio_covers_interfaces_0_to_2", n_iad >= 1 && iad_first[0] == 0 && iad_cnt[0] == 3, "first function");
    CHECK("iad_midi_covers_interfaces_3_to_4", n_iad >= 2 && iad_first[1] == 3 && iad_cnt[1] == 2, "second function (stock: MIDI is MI_03)");
    CHECK("iads_cover_every_interface_once", n_iad == 2 && iad_cnt[0] + iad_cnt[1] == n_itf && iad_first[0] + iad_cnt[0] == iad_first[1], "overlap or gap");

    /* ---- audio control ---- */
    CHECK("two_audio_control_headers", ac_n == 2, "one for the audio function, one for the MIDI function");
    if (ac_n == 2) {
        CHECK("ac_audio_lists_both_streaming_interfaces", ac_itf_of[0] == 0 && ac_list_n[0] == 2 && ac_list[0][0] == 1 && ac_list[0][1] == 2, "baInterfaceNr");
        CHECK("ac_audio_total_length_is_exact", ac_hdr_total[0] == ac_hdr_calc[0], "wTotalLength of the audio control block");
        CHECK("ac_midi_lists_the_midi_streaming_interface", ac_itf_of[1] == 3 && ac_list_n[1] == 1 && ac_list[1][0] == 4, "baInterfaceNr");
        CHECK("ac_midi_total_length_is_exact", ac_hdr_total[1] == ac_hdr_calc[1], "wTotalLength of the MIDI control block");
        int ok = 1;
        for (int k = 0; k < 2; k++) for (int i = 0; i < ac_list_n[k]; i++) { int t = ac_list[k][i]; if (t >= 8 || !itf[t].seen) ok = 0; }
        CHECK("ac_listed_interfaces_exist", ok, "a baInterfaceNr names an interface that is not there");
    }
    /* terminals: unique ids, links resolve to streaming terminals */
    int uniq = 1; for (int i = 0; i < n_term; i++) for (int j = i + 1; j < n_term; j++) if (term_id[i] == term_id[j]) uniq = 0;
    CHECK("terminal_ids_unique", uniq, "duplicate terminal id");
    CHECK("four_terminals", n_term == 4, "two for the speaker path, two for the microphone path");
    int link_ok = (n_as == 2);
    for (int i = 0; i < n_as && link_ok; i++) {
        int found = 0;
        for (int t = 0; t < n_term; t++) if (term_id[t] == as_link[i]) {
            found = 1;
            /* playback links to an INPUT terminal of type USB streaming, recording to an OUTPUT terminal of that type */
            if (term_type[t] != 0x0101) link_ok = 0;
            if (as_itf[i] == 1 && term_dir[t] != 1) link_ok = 0;
            if (as_itf[i] == 2 && term_dir[t] != 2) link_ok = 0;
        }
        if (!found) link_ok = 0;
    }
    CHECK("streaming_interfaces_link_to_the_right_terminals", link_ok, "AS_GENERAL bTerminalLink");

    /* ---- format: the stock's, both streams ---- */
    CHECK("both_formats_equal_the_stock_24bit_44k1_stereo", fmt_n == 2 && fmt_ok == 2, "format type I descriptor differs from the stock's 0B 24 02 01 02 03 18 01 44 AC 00");

    /* ---- endpoints ---- */
    int ep1o = -1, ep1i = -1, ep3o = -1, ep3i = -1;
    for (int i = 0; i < n_ep; i++) { if (ep_addr[i] == 0x01) ep1o = i; if (ep_addr[i] == 0x81) ep1i = i; if (ep_addr[i] == 0x03) ep3o = i; if (ep_addr[i] == 0x83) ep3i = i; }
    CHECK("four_endpoints_one_per_direction_and_number", n_ep == 4 && ep1o >= 0 && ep1i >= 0 && ep3o >= 0 && ep3i >= 0, "endpoint set");
    if (n_ep == 4 && ep1o >= 0 && ep1i >= 0 && ep3o >= 0 && ep3i >= 0) {
        CHECK("midi_endpoints_are_bulk_64_on_interface_4", ep_attr[ep1o] == 2 && ep_attr[ep1i] == 2 && ep_maxp[ep1o] == 64 && ep_maxp[ep1i] == 64 && ep_itf[ep1o] == 4 && ep_itf[ep1i] == 4, "MIDI endpoints must be as before");
        CHECK("playback_endpoint_is_isochronous_adaptive_out", ep_attr[ep3o] == 0x09 && ep_itf[ep3o] == 1, "stock: 0x09 on endpoint 3 OUT");
        CHECK("recording_endpoint_is_isochronous_async_in", ep_attr[ep3i] == 0x05 && ep_itf[ep3i] == 2, "stock: 0x05 on endpoint 0x83");
        CHECK("iso_max_packet_holds_46_stereo_24bit_frames", ep_maxp[ep3o] >= 46 * 6 && ep_maxp[ep3i] >= 46 * 6 && ep_maxp[ep3o] <= 1023 && ep_maxp[ep3i] <= 1023, "full speed isochronous limit is 1023");
        CHECK("iso_max_packet_at_least_the_stocks", ep_maxp[ep3o] >= 274 && ep_maxp[ep3i] >= 270, "stock: 274 out, 270 in");
    }

    /* ---- MIDI streaming ---- */
    CHECK("midi_ms_total_length_is_exact", ms_total == ms_calc, "the class-specific MIDI block does not add up to its header's wTotalLength");
    CHECK("midi_ms_total_is_65_as_before", ms_total == 65, "the MIDI streaming block was 65 bytes");
    CHECK("string_indexes_point_at_strings_1_and_2", PA_DEV_DESC[14] == 1 && PA_DEV_DESC[15] == 2, "iManufacturer / iProduct");

    printf("COMMS-SUITE|usb_desc|passed=%d|failed=%d|known=0\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

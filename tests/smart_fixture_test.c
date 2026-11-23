/*
 * SMART display test on fabricated data (no disk needed).
 * Compiles diskmedic.c in the same translation unit to reach its internal
 * functions: smart_print_ata(), nvme_print() and parse_ata_sense().
 */
#define main diskmedic_main
#include "../diskmedic.c"
#undef main

/* ATA IDENTIFY string: padded with spaces, bytes swapped in pairs. */
static void put_ata_string(unsigned char *id, size_t off, size_t len, const char *s) {
    for (size_t i = 0; i < len; i++) {
        char c = i < strlen(s) ? s[i] : ' ';
        id[off + (i ^ 1)] = (unsigned char)c;
    }
}
static void put_word(unsigned char *id, unsigned w, uint16_t v) { id[w * 2] = (unsigned char)v; id[w * 2 + 1] = (unsigned char)(v >> 8); }
static void put_attr(unsigned char *data, unsigned slot, uint8_t aid, uint8_t cur, uint8_t worst, uint64_t raw) {
    unsigned char *a = data + 2 + slot * 12;
    a[0] = aid; a[1] = 0x33; a[2] = 0; a[3] = cur; a[4] = worst;
    for (int k = 0; k < 6; k++) a[5 + k] = (unsigned char)(raw >> (8 * k));
}
static void put_thr(unsigned char *thr, unsigned slot, uint8_t aid, uint8_t t) { thr[2 + slot * 12] = aid; thr[2 + slot * 12 + 1] = t; }
static void put_le(unsigned char *p, unsigned n, uint64_t v) { for (unsigned i = 0; i < n; i++) p[i] = (unsigned char)(v >> (8 * i)); }

/* LOG SENSE page: 4-byte header, then parameters added by log_param(). */
static unsigned log_page_begin(unsigned char *pg, uint8_t code) { memset(pg, 0, 512); pg[0] = code; return 4; }
static unsigned log_param(unsigned char *pg, unsigned pos, uint16_t pc, unsigned len, uint64_t v) {
    pg[pos] = (unsigned char)(pc >> 8); pg[pos + 1] = (unsigned char)pc; pg[pos + 2] = 0x03; pg[pos + 3] = (unsigned char)len;
    for (unsigned i = 0; i < len; i++) pg[pos + 4 + i] = (unsigned char)(v >> (8 * (len - 1 - i)));
    return pos + 4 + len;
}
static void log_page_end(unsigned char *pg, unsigned pos, sas_info_t *si) {
    pg[2] = (unsigned char)((pos - 4) >> 8); pg[3] = (unsigned char)(pos - 4);
    sas_parse_page(si, pg, pos);
}
static void sas_fixture(sas_info_t *si, int failing) {
    unsigned char pg[512]; unsigned p;
    sas_init(si);
    snprintf(si->vendor, sizeof(si->vendor), "SEAGATE"); snprintf(si->product, sizeof(si->product), "ST4000NM0023");
    snprintf(si->rev, sizeof(si->rev), "0004"); snprintf(si->serial, sizeof(si->serial), "Z1Z2ABCD");
    si->rotation = 7200;
    p = log_page_begin(pg, 0x2f); p = log_param(pg, p, 0, 3, failing ? 0x5d1029 : 0x000029); log_page_end(pg, p, si);
    p = log_page_begin(pg, 0x0d); p = log_param(pg, p, 0, 2, 41); p = log_param(pg, p, 1, 2, 60); log_page_end(pg, p, si);
    p = log_page_begin(pg, 0x03); p = log_param(pg, p, 3, 4, 1234); p = log_param(pg, p, 5, 8, UINT64_C(12300000000000));
    p = log_param(pg, p, 6, 4, failing ? 2 : 0); log_page_end(pg, p, si);
    p = log_page_begin(pg, 0x02); p = log_param(pg, p, 3, 4, 10); p = log_param(pg, p, 5, 8, UINT64_C(48200000000000));
    p = log_param(pg, p, 6, 4, 0); log_page_end(pg, p, si);
    p = log_page_begin(pg, 0x0e);
    pg[p] = 0; pg[p + 1] = 1; pg[p + 2] = 0x03; pg[p + 3] = 6; memcpy(pg + p + 4, "201612", 6); p += 10;
    p = log_param(pg, p, 3, 4, 50000); p = log_param(pg, p, 4, 4, 1204); log_page_end(pg, p, si);
    p = log_page_begin(pg, 0x15); p = log_param(pg, p, 0, 4, UINT64_C(23456) * 60); log_page_end(pg, p, si);
    unsigned char defects[4] = {0, 0x0c, 0, (unsigned char)(failing ? 96 : 0)};   /* grown list, format 4, 8 B/defect */
    sas_parse_defects(si, defects, 4);
}

int main(void) {
    unsigned char id[512], data[512], thr[512];
    uint64_t lba48 = UINT64_C(7814037168);

    /* 1. 4 TB hard disk in poor condition. */
    memset(id, 0, sizeof(id)); memset(data, 0, sizeof(data)); memset(thr, 0, sizeof(thr));
    put_ata_string(id, 20, 20, "WD-WCC7K1234567");
    put_ata_string(id, 46, 8, "82.00A82");
    put_ata_string(id, 54, 40, "WDC WD40EFRX-68N32N0");
    for (unsigned w = 0; w < 4; w++) put_word(id, 100 + w, (uint16_t)(lba48 >> (16 * w)));
    put_word(id, 82, 1); put_word(id, 85, 1); put_word(id, 217, 5400);
    put_word(id, 76, 0x0e); put_word(id, 77, 2 << 1);        /* supports 6 Gb/s, link at 3 Gb/s */
    put_word(id, 128, 0x21);
    put_attr(data, 0, 1, 200, 200, 0);         put_thr(thr, 0, 1, 51);
    put_attr(data, 1, 3, 180, 175, 6025);      put_thr(thr, 1, 3, 21);
    put_attr(data, 2, 5, 200, 200, 8);         put_thr(thr, 2, 5, 140);
    put_attr(data, 3, 9, 68, 68, 23456);       put_thr(thr, 3, 9, 0);
    put_attr(data, 4, 10, 100, 40, 0);         put_thr(thr, 4, 10, 51);
    put_attr(data, 5, 12, 100, 100, 1204);     put_thr(thr, 5, 12, 0);
    put_attr(data, 6, 194, 111, 95, 41 | (UINT64_C(20) << 16) | (UINT64_C(52) << 32)); put_thr(thr, 6, 194, 0);
    put_attr(data, 7, 197, 200, 200, 3);       put_thr(thr, 7, 197, 0);
    put_attr(data, 8, 199, 200, 200, 12);      put_thr(thr, 8, 199, 0);
    printf("=== HDD\n"); fflush(stdout);
    smart_print_ata(id, data, thr, 1, 0);

    /* 2. Healthy SATA SSD. */
    memset(id, 0, sizeof(id)); memset(data, 0, sizeof(data));
    put_ata_string(id, 20, 20, "S6PNNX0T123456");
    put_ata_string(id, 46, 8, "SVT02B6Q");
    put_ata_string(id, 54, 40, "Samsung SSD 870 EVO 1TB");
    put_word(id, 82, 1); put_word(id, 85, 1); put_word(id, 217, 1);
    put_word(id, 76, 0x0e); put_word(id, 77, 3 << 1);
    put_attr(data, 0, 5, 100, 100, 0);
    put_attr(data, 1, 9, 95, 95, 23456);
    put_attr(data, 2, 12, 99, 99, 1204);
    put_attr(data, 3, 177, 92, 92, 160);
    put_attr(data, 4, 190, 63, 52, 37);
    put_attr(data, 5, 241, 99, 99, UINT64_C(94140625000));
    printf("=== SSD\n"); fflush(stdout);
    smart_print_ata(id, data, NULL, -1, UINT64_C(1000204886016));

    /* 3. Healthy NVMe, 4. failing NVMe. */
    unsigned char ctrl[4096], log[512];
    memset(ctrl, 0, sizeof(ctrl)); memset(log, 0, sizeof(log));
    memcpy(ctrl + 4, "S4EWNX0R123456      ", 20);
    memcpy(ctrl + 24, "Samsung SSD 980 PRO 1TB                 ", 40);
    memcpy(ctrl + 64, "5B2QGXA7", 8);
    put_le(ctrl + 266, 2, 353); put_le(ctrl + 268, 2, 358);
    put_le(log + 1, 2, 314); log[3] = 100; log[4] = 10; log[5] = 3;
    put_le(log + 32, 8, 24070312); put_le(log + 48, 8, 94140625);
    put_le(log + 112, 8, 1204); put_le(log + 128, 8, 23456); put_le(log + 144, 8, 12);
    printf("=== NVME\n"); fflush(stdout);
    nvme_print(ctrl, log, UINT64_C(1000204886016));
    log[0] = 0x04; log[5] = 95; put_le(log + 160, 8, 5);
    printf("=== NVME_BAD\n"); fflush(stdout);
    nvme_print(ctrl, log, UINT64_C(1000204886016));

    /* 5. Failing SAS, healthy SAS, SAS without any health data. */
    sas_info_t si;
    sas_fixture(&si, 1);
    printf("=== SAS_BAD\n"); fflush(stdout);
    sas_print(&si, UINT64_C(4000787030016));
    sas_fixture(&si, 0);
    printf("=== SAS\n"); fflush(stdout);
    sas_print(&si, UINT64_C(4000787030016));
    sas_init(&si);
    snprintf(si.vendor, sizeof(si.vendor), "JMicron"); snprintf(si.product, sizeof(si.product), "Generic");
    printf("=== SAS_NONE\n"); fflush(stdout);
    sas_print(&si, 0);

    /* 6. Decoding of the SAT registers (SMART RETURN STATUS). */
    unsigned char desc[22] = {0x72, 0x01, 0x00, 0x1d, 0, 0, 0, 14,
                              0x09, 0x0c, 0, 0, 0, 0, 0, 0, 0, 0x4f, 0, 0xc2, 0, 0x50};
    unsigned char fixed[18] = {0x70, 0, 0x01, 0, 0x50, 0, 0, 10, 0, 0, 0xf4, 0x2c, 0x00, 0x1d, 0, 0, 0, 0};
    ata_regs_t r;
    printf("=== SENSE\n");
    int ok = parse_ata_sense(desc, sizeof(desc), &r);
    printf("desc valid=%d mid=%02x high=%02x\n", ok, r.lba_mid, r.lba_high);
    ok = parse_ata_sense(fixed, sizeof(fixed), &r);
    printf("fixed valid=%d mid=%02x high=%02x\n", ok, r.lba_mid, r.lba_high);

    /* 7. Choice of the NVMe Secure Erase method (Identify Controller: OACS, SANICAP, FNA). */
    static const struct { const char *name; uint16_t oacs; uint32_t sanicap; uint8_t fna; int ns; } ec[] = {
        {"crypto+block", 0x0002, 0x3, 0x00, 1},
        {"block", 0x0002, 0x2, 0x00, 1},
        {"format-crypto", 0x0002, 0x0, 0x04, 1},
        {"format", 0x0002, 0x0, 0x00, 1},
        {"multi-ns", 0x0002, 0x3, 0x00, 2},
        {"multi-ns-format-global", 0x0002, 0x3, 0x02, 2},
        {"ns-unknown", 0x0002, 0x1, 0x00, -1},
        {"overwrite-only", 0x0000, 0x4, 0x00, 1},
        {"nothing", 0x0000, 0x0, 0x00, 1},
    };
    printf("=== ERASE\n");
    for (size_t i = 0; i < sizeof(ec) / sizeof(ec[0]); i++) {
        unsigned char ctrl[4096] = {0};
        nvme_plan_t plan;
        put_le(ctrl + 256, 2, ec[i].oacs);
        put_le(ctrl + 328, 4, ec[i].sanicap);
        ctrl[524] = ec[i].fna;
        int rc = nvme_erase_choose(ctrl, ec[i].ns, &plan);
        printf("%s: %d %s | %s\n", ec[i].name, rc, nvme_method_name(plan.method), plan.why);
    }
    return 0;
}

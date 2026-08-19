/*
 * kbled — 神舟/蓝天 (Clevo NH5x/NH7x) 笔记本键盘背光控制工具
 *
 * 通过 ACPI EC 命令邮箱 (EC 偏移 0xF8-0xFD) 控制键盘背光。
 * 协议依据 gigactl (https://github.com/smairio/gigactl) 逆向的 Clevo EC 协议，
 * 已在 HASEE NH5x_NH7xHP (Ubuntu 26.04, kernel 7.0) 上实测验证。
 *
 * 用法:
 *   kbled on               开启背光（应用保存的颜色与亮度）
 *   kbled off              关闭背光
 *   kbled color RRGGBB     设置颜色（十六进制，如 FF0000 为红）
 *   kbled brightness N     设置亮度 0-255
 *   kbled up               亮度加一档 (+25)
 *   kbled down             亮度减一档 (-25)
 *   kbled toggle           背光开/关切换
 *   kbled cycle            循环切换预设颜色
 *   kbled apply            应用配置文件（供开机自启服务调用）
 *   kbled status           显示当前配置与 EC 状态
 *
 * up/down/toggle/cycle 供桌面快捷键绑定使用（本机 Fn 热键为固件死键，
 * 无事件信号，用可捕获的按键组合 + 本命令实现等效控制）。
 *
 * 配置文件: /etc/kbled.conf
 * 需要 root 权限（建议通过 sudoers NOPASSWD 规则调用）。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <sys/io.h>

#define CONFIG_PATH "/etc/kbled.conf"
#define EC_IO_PATH  "/sys/kernel/debug/ec/ec0/io"

/* ACPI EC 端口 */
#define EC_SC   0x66
#define EC_DATA 0x62

/* 配置文件字段 */
static int  cfg_enabled   = 1;
static int  cfg_color[3]  = {255, 255, 255}; /* R,G,B */
static int  cfg_bright    = 200;

static int ec_fd = -1;
static int ec_use_ports = 0;

/* ---------------- EC 访问 ---------------- */

static int ec_init(void) {
    /* 优先使用内核 ec_sys 调试接口（无需端口抢占） */
    ec_fd = open(EC_IO_PATH, O_RDWR);
    if (ec_fd >= 0) return 0;

    /* 尝试加载 ec_sys 模块并挂载 debugfs */
    system("modprobe ec_sys write_support=1 2>/dev/null");
    system("mount -t debugfs none /sys/kernel/debug 2>/dev/null");
    ec_fd = open(EC_IO_PATH, O_RDWR);
    if (ec_fd >= 0) return 0;

    /* 回退：直接访问 EC 端口 (需要 CAP_SYS_RAWIO) */
    if (ioperm(EC_DATA, 5, 1) == 0) { ec_use_ports = 1; return 0; }
    return -1;
}

static int ec_wait_ibf(void) {
    for (int i = 0; i < 20000; i++)
        if (!(inb(EC_SC) & 0x02)) return 0;
    return -1;
}
static int ec_wait_obf(void) {
    for (int i = 0; i < 20000; i++)
        if (inb(EC_SC) & 0x01) return 0;
    return -1;
}

static int ec_write_byte(int addr, unsigned char val) {
    if (ec_use_ports) {
        if (ec_wait_ibf()) return -1;
        outb(0x81, EC_SC);                 /* 写 EC RAM 命令 */
        if (ec_wait_ibf()) return -1;
        outb((unsigned char)addr, EC_DATA);
        if (ec_wait_ibf()) return -1;
        outb(val, EC_DATA);
        return 0;
    }
    return (pwrite(ec_fd, &val, 1, (off_t)addr) == 1) ? 0 : -1;
}

static int ec_read_byte(int addr, unsigned char *out) {
    if (ec_use_ports) {
        if (ec_wait_ibf()) return -1;
        outb(0x80, EC_SC);                 /* 读 EC RAM 命令 */
        if (ec_wait_ibf()) return -1;
        outb((unsigned char)addr, EC_DATA);
        if (ec_wait_obf()) return -1;
        *out = inb(EC_DATA);
        return 0;
    }
    return (pread(ec_fd, out, 1, (off_t)addr) == 1) ? 0 : -1;
}

/* ---------------- EC 命令原语 ---------------- */

/* 清空命令参数区 */
static void ec_clear_params(void) {
    ec_write_byte(0xF9, 0);
    ec_write_byte(0xFA, 0);
    ec_write_byte(0xFB, 0);
    ec_write_byte(0xFC, 0);
    ec_write_byte(0xFD, 0);
}

/* 键盘状态主使能（开机后必须先发，否则 EC 忽略所有背光命令） */
static void kbd_master_enable(void) {
    ec_clear_params();
    ec_write_byte(0xF9, 0x0C);
    ec_write_byte(0xFA, 0x3F);
    ec_write_byte(0xF8, 0xC4);   /* doorbell, 最后写 */
}

/* 键盘状态主禁用 */
static void kbd_master_disable(void) {
    ec_clear_params();
    ec_write_byte(0xF9, 0x0C);
    ec_write_byte(0xFA, 0x20);
    ec_write_byte(0xF8, 0xC4);
}

/* 设置整键盘颜色（B,R,G 顺序！），z 为分区 0..2，本机分区并联视为整键盘 */
static void kbd_set_color_zone(int z, int r, int g, int b) {
    ec_clear_params();
    ec_write_byte(0xF9, (unsigned char)(0x03 + z)); /* 分区 0/1/2 */
    ec_write_byte(0xFA, (unsigned char)b);           /* Blue  */
    ec_write_byte(0xFB, (unsigned char)r);           /* Red   */
    ec_write_byte(0xFC, (unsigned char)g);           /* Green */
    ec_write_byte(0xF8, 0xCA);
}

static void kbd_set_brightness(int v) {
    ec_clear_params();
    ec_write_byte(0xF9, 0x06);
    ec_write_byte(0xFA, (unsigned char)v);
    ec_write_byte(0xF8, 0xCA);
}

/* ---------------- 配置读写 ---------------- */

static void cfg_save(void) {
    FILE *f = fopen(CONFIG_PATH, "w");
    if (!f) { perror("fopen config"); return; }
    fprintf(f, "enabled=%d\n", cfg_enabled);
    fprintf(f, "color=%02X%02X%02X\n", cfg_color[0], cfg_color[1], cfg_color[2]);
    fprintf(f, "brightness=%d\n", cfg_bright);
    fclose(f);
}

static void cfg_load(void) {
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) return;  /* 首次运行用默认值 */
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "enabled=%d", &cfg_enabled) == 1) continue;
        if (sscanf(line, "brightness=%d", &cfg_bright) == 1) continue;
        if (sscanf(line, "color=%2x%2x%2x",
                   (unsigned int *)&cfg_color[0],
                   (unsigned int *)&cfg_color[1],
                   (unsigned int *)&cfg_color[2]) == 3) continue;
    }
    fclose(f);
}

/* ---------------- 动作 ---------------- */

static void apply_settings(void) {
    if (!cfg_enabled) { kbd_master_disable(); return; }
    kbd_master_enable();
    usleep(50000);
    /* 三个分区都设置同一颜色（本机为单区，重复设置无害且更稳） */
    for (int z = 0; z < 3; z++)
        kbd_set_color_zone(z, cfg_color[0], cfg_color[1], cfg_color[2]);
    usleep(20000);
    kbd_set_brightness(cfg_bright);
}

static void print_status(void) {
    printf("配置文件: %s\n", CONFIG_PATH);
    printf("启用:     %s\n", cfg_enabled ? "是" : "否");
    printf("颜色:     #%02X%02X%02X\n", cfg_color[0], cfg_color[1], cfg_color[2]);
    printf("亮度:     %d/255\n", cfg_bright);
    unsigned char v;
    printf("EC 命令区: FCMD=");
    if (ec_read_byte(0xF8, &v) == 0) printf("0x%02X", v); else printf("?");
    printf("  FDAT=");
    if (ec_read_byte(0xF9, &v) == 0) printf("0x%02X", v); else printf("?");
    printf("  FBUF=");
    if (ec_read_byte(0xFA, &v) == 0) printf("0x%02X", v); else printf("?");
    printf("\n");
}

static int parse_hex_color(const char *s, int *r, int *g, int *b) {
    unsigned int c;
    if (strlen(s) != 6) return -1;
    for (int i = 0; i < 6; i++)
        if (!isxdigit((unsigned char)s[i])) return -1;
    if (sscanf(s, "%6x", &c) != 1) return -1;
    *r = (c >> 16) & 0xFF;
    *g = (c >> 8) & 0xFF;
    *b = c & 0xFF;
    return 0;
}

static void usage(const char *prog) {
    fprintf(stderr,
        "用法: %s <命令> [参数]\n"
        "  on                 开启背光（应用保存的颜色与亮度）\n"
        "  off                关闭背光\n"
        "  color RRGGBB       设置颜色（如 FF0000=红, 00FF00=绿, 0000FF=蓝）\n"
        "  brightness N       设置亮度 0-255\n"
        "  up                 亮度加一档 (+25)\n"
        "  down               亮度减一档 (-25)\n"
        "  toggle             背光开/关切换\n"
        "  cycle              循环切换预设颜色\n"
        "  apply              应用配置文件（开机自启服务调用）\n"
        "  status             显示当前配置与 EC 状态\n", prog);
}

/* 循环预设色板 */
static const unsigned int palette[] = {
    0xFFFFFF, 0x00C8FF, 0xFF0000, 0x00FF00,
    0x0000FF, 0xFFFF00, 0xFF00FF, 0x00FFFF,
};
#define PALETTE_N (sizeof(palette) / sizeof(palette[0]))

static void do_cycle(void) {
    unsigned int cur = ((unsigned int)cfg_color[0] << 16)
                     | ((unsigned int)cfg_color[1] << 8)
                     | (unsigned int)cfg_color[2];
    size_t idx = 0;
    for (size_t i = 0; i < PALETTE_N; i++)
        if (palette[i] == cur) { idx = (i + 1) % PALETTE_N; break; }
    cfg_color[0] = (palette[idx] >> 16) & 0xFF;
    cfg_color[1] = (palette[idx] >> 8) & 0xFF;
    cfg_color[2] = palette[idx] & 0xFF;
    cfg_save();
    if (cfg_enabled) apply_settings();
    printf("颜色已循环至 #%02X%02X%02X\n", cfg_color[0], cfg_color[1], cfg_color[2]);
}

#define BRIGHT_STEP 25
static void do_bright_up(void) {
    if (cfg_bright + BRIGHT_STEP > 255) cfg_bright = 255;
    else cfg_bright += BRIGHT_STEP;
    cfg_save();
    if (cfg_enabled) apply_settings();
    printf("亮度 %d/255\n", cfg_bright);
}
static void do_bright_down(void) {
    if (cfg_bright - BRIGHT_STEP < 0) cfg_bright = 0;
    else cfg_bright -= BRIGHT_STEP;
    cfg_save();
    if (cfg_enabled) apply_settings();
    printf("亮度 %d/255\n", cfg_bright);
}
static void do_toggle(void) {
    if (cfg_enabled) {
        cfg_enabled = 0;
        cfg_save();
        kbd_master_disable();
        printf("键盘背光已关闭\n");
    } else {
        cfg_enabled = 1;
        cfg_save();
        apply_settings();
        printf("键盘背光已开启 (#%02X%02X%02X, 亮度 %d)\n",
               cfg_color[0], cfg_color[1], cfg_color[2], cfg_bright);
    }
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(argv[0]); return 2; }

    if (ec_init() != 0) {
        fprintf(stderr, "错误: 无法访问 EC (需要 root 权限或加载 ec_sys 模块)\n");
        return 1;
    }
    cfg_load();

    const char *cmd = argv[1];
    if (strcmp(cmd, "status") == 0) {
        print_status();
        return 0;
    }
    if (strcmp(cmd, "on") == 0) {
        cfg_enabled = 1;
        cfg_save();
        apply_settings();
        printf("键盘背光已开启 (#%02X%02X%02X, 亮度 %d)\n",
               cfg_color[0], cfg_color[1], cfg_color[2], cfg_bright);
        return 0;
    }
    if (strcmp(cmd, "off") == 0) {
        cfg_enabled = 0;
        cfg_save();
        kbd_master_disable();
        printf("键盘背光已关闭\n");
        return 0;
    }
    if (strcmp(cmd, "color") == 0 && argc >= 3) {
        int r, g, b;
        if (parse_hex_color(argv[2], &r, &g, &b) != 0) {
            fprintf(stderr, "颜色格式错误: 应为 6 位十六进制 (如 FF0000)\n");
            return 1;
        }
        cfg_color[0] = r; cfg_color[1] = g; cfg_color[2] = b;
        cfg_save();
        if (cfg_enabled) apply_settings();
        printf("颜色已设为 #%02X%02X%02X\n", r, g, b);
        return 0;
    }
    if (strcmp(cmd, "brightness") == 0 && argc >= 3) {
        int v = atoi(argv[2]);
        if (v < 0 || v > 255) { fprintf(stderr, "亮度范围: 0-255\n"); return 1; }
        cfg_bright = v;
        cfg_save();
        if (cfg_enabled) apply_settings();
        printf("亮度已设为 %d\n", v);
        return 0;
    }
    if (strcmp(cmd, "apply") == 0) {
        apply_settings();
        printf("已应用配置 (%s, #%02X%02X%02X, 亮度 %d)\n",
               cfg_enabled ? "开启" : "关闭",
               cfg_color[0], cfg_color[1], cfg_color[2], cfg_bright);
        return 0;
    }
    if (strcmp(cmd, "up") == 0)   { do_bright_up();   return 0; }
    if (strcmp(cmd, "down") == 0) { do_bright_down(); return 0; }
    if (strcmp(cmd, "toggle") == 0){ do_toggle();      return 0; }
    if (strcmp(cmd, "cycle") == 0) { do_cycle();       return 0; }

    usage(argv[0]);
    return 2;
}
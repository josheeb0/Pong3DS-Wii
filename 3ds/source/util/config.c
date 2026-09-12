#include "config.h"

#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

static void trim(char *s)
{
    char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' ||
                     s[n - 1] == ' '  || s[n - 1] == '\t')) {
        s[--n] = '\0';
    }
}

static void apply(PongConfig *cfg, const char *key, const char *val)
{
    if      (!strcmp(key, "lan_host"))   snprintf(cfg->net.lan_host, sizeof cfg->net.lan_host, "%s", val);
    else if (!strcmp(key, "lan_port"))   cfg->net.lan_port = (uint16_t)atoi(val);
    else if (!strcmp(key, "lan_subnet")) snprintf(cfg->net.lan_subnet, sizeof cfg->net.lan_subnet, "%s", val);
    else if (!strcmp(key, "web_host"))   snprintf(cfg->net.web_host, sizeof cfg->net.web_host, "%s", val);
    else if (!strcmp(key, "web_port"))   cfg->net.web_port = (uint16_t)atoi(val);
    else if (!strcmp(key, "web_tls"))    cfg->net.web_tls = atoi(val) != 0;
    else if (!strcmp(key, "web_verify")) cfg->net.web_verify = atoi(val) != 0;
    else if (!strcmp(key, "web_path"))   snprintf(cfg->net.web_path, sizeof cfg->net.web_path, "%s", val);
    else if (!strcmp(key, "name"))       snprintf(cfg->player_name, sizeof cfg->player_name, "%s", val);
    else if (!strcmp(key, "autoupdate")) cfg->autoupdate = atoi(val) != 0;
    else if (!strcmp(key, "gh_owner"))   snprintf(cfg->gh_owner, sizeof cfg->gh_owner, "%s", val);
    else if (!strcmp(key, "gh_repo"))    snprintf(cfg->gh_repo, sizeof cfg->gh_repo, "%s", val);
    else if (!strcmp(key, "update_source")) {
        cfg->update_source = strcmp(val, "github") == 0
            ? PONG_UPDATE_SRC_GITHUB : PONG_UPDATE_SRC_SERVER;
    }
    else if (!strcmp(key, "mode")) {
        if      (!strcmp(val, "lan")) cfg->net.mode = PONG_MODE_LAN;
        else if (!strcmp(val, "web")) cfg->net.mode = PONG_MODE_WEB;
        else                          cfg->net.mode = PONG_MODE_AUTO;
    }
}

static void parse_file(PongConfig *cfg, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[192];
    while (fgets(line, sizeof line, f)) {
        trim(line);
        if (line[0] == '\0' || line[0] == '#') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = line;
        char *val = eq + 1;
        trim(key);
        trim(val);
        apply(cfg, key, val);
    }
    fclose(f);
}

void pong_config_load(PongConfig *cfg)
{
    memset(cfg, 0, sizeof *cfg);

    /*
     * Compiled-in fallbacks, so the app still runs if romfs is missing.
     *
     * lan_host is EMPTY by default. The LAN path is a raw TCP socket on its own
     * port, which cannot share 443 with HTTPS -- so assuming a second port
     * exists would be wrong for any deployment that only publishes 443. It is
     * an opt-in optimisation: set lan_host to enable it.
     */
    cfg->net.lan_host[0] = '\0';
    cfg->net.lan_port = 8787;
    cfg->net.lan_subnet[0] = '\0';
    snprintf(cfg->net.web_host, sizeof cfg->net.web_host, "pong.wardcrew.com");
    cfg->net.web_port = 443;
    cfg->net.web_tls = true;
    cfg->net.web_verify = true;   /* secure by default; see https.h */
    snprintf(cfg->net.web_path, sizeof cfg->net.web_path, "/api/rpc");
    cfg->net.mode = PONG_MODE_AUTO;
    snprintf(cfg->player_name, sizeof cfg->player_name, "3DS PLAYER");
    cfg->autoupdate = true;
    /* Defaults to the upstream repository's releases: independent of whether
     * any particular server has been redeployed, which is the gap the server
     * source cannot see. Cycle the SOURCE row to pick a different one. */
    cfg->update_source = PONG_UPDATE_SRC_GITHUB;
    snprintf(cfg->gh_owner, sizeof cfg->gh_owner, "%s", PONG_GH_OWNER);
    snprintf(cfg->gh_repo, sizeof cfg->gh_repo, "%s", PONG_GH_REPO);

    parse_file(cfg, "romfs:/config.txt");
    parse_file(cfg, PONG_SD_CONFIG);   /* SD wins */
}

void pong_config_save(const PongConfig *cfg)
{
    pong_config_remember_mode(cfg, cfg->net.mode);
}

void pong_config_remember_mode(const PongConfig *cfg, PongNetMode mode)
{
    /* Only written when it would change the answer, to avoid an SD write on
     * every launch. */
    FILE *f = fopen(PONG_SD_CONFIG, "w");
    if (!f) return;
    fprintf(f, "# written by Pong3DS -- edit freely, these override romfs defaults\n");
    fprintf(f, "lan_host=%s\n", cfg->net.lan_host);
    fprintf(f, "lan_port=%u\n", (unsigned)cfg->net.lan_port);
    fprintf(f, "lan_subnet=%s\n", cfg->net.lan_subnet);
    fprintf(f, "web_host=%s\n", cfg->net.web_host);
    fprintf(f, "web_port=%u\n", (unsigned)cfg->net.web_port);
    fprintf(f, "web_tls=%d\n", cfg->net.web_tls ? 1 : 0);
    fprintf(f, "web_verify=%d\n", cfg->net.web_verify ? 1 : 0);
    fprintf(f, "web_path=%s\n", cfg->net.web_path);
    fprintf(f, "name=%s\n", cfg->player_name);
    fprintf(f, "autoupdate=%d\n", cfg->autoupdate ? 1 : 0);
    fprintf(f, "update_source=%s\n",
            cfg->update_source == PONG_UPDATE_SRC_GITHUB ? "github" : "server");
    fprintf(f, "gh_owner=%s\n", cfg->gh_owner);
    fprintf(f, "gh_repo=%s\n", cfg->gh_repo);
    fprintf(f, "mode=%s\n", mode == PONG_MODE_LAN ? "lan"
                          : mode == PONG_MODE_WEB ? "web" : "auto");
    fclose(f);
}

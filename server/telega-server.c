#include <stdbool.h>
#include <pthread.h>
#include <assert.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>

#include <td/telegram/td_json_client.h>
#include <td/telegram/td_log.h>

#include "telega-dat.h"

/*
 * Input/Output Protocol:
 * ~~~~~~~~~~~~~~
 *   <COMMAND> <SPACE> <PLIST-LEN> <NEWLINE>
 *   <PLIST of PLIST-LEN length> <NEWLINE>
 *
 * COMMAND is one of `send', `event' or `error'
 * `event' and `error' is used for output
 *
 * For example:
 *   event 105
 *   (:@type "updateAuthorizationState" :authorization_state (:@type "authorizationStateWaitTdlibParameters"))
 *
 *   send 109
 *   (:@type "getTextEntities" :text "@telegram /test_command https://telegram.org telegram.me" :@extra ["5" 7.0])
 *
 */

char* logfile = NULL;
int verbosity = 5;
const char* version = "0.2.0";

int parse_mode = 0;
#define PARSE_MODE_JSON 1
#define PARSE_MODE_PLIST 2

void
usage(char* prog)
{
        printf("Version %s\n", version);
        printf("usage: %s [-jp] [-l FILE] [-v LVL] [-h]\n", prog);
        printf("\t-l FILE    Log to FILE (default=stderr)\n");
        printf("\t-v LVL     Verbosity level (default=5)\n");
        printf("\n-j         Parse json from stdin and exit\n");
        printf("\n-p         Parse plist from stdin and exit\n");
        exit(0);
}

static void
on_error_cb(const char* errmsg)
{
        struct telega_dat json_src = TDAT_INIT;
        struct telega_dat plist_dst = TDAT_INIT;

        tdat_append(&json_src, errmsg, strlen(errmsg));
        tdat_json_value(&json_src, &plist_dst);
        tdat_append1(&plist_dst, "\0");

        printf("error %zu\n%s\n", strlen(plist_dst.data), plist_dst.data);
        fflush(stdout);

        tdat_drop(&json_src);
        tdat_drop(&plist_dst);
}

static void*
tdlib_loop(void* cln)
{
        struct telega_dat json_src = TDAT_INIT;
        struct telega_dat plist_dst = TDAT_INIT;

        while (true) {
                const char *res = td_json_client_receive(cln, 1);
                if (res) {
                        tdat_append(&json_src, res, strlen(res));
                        fprintf(stderr, "IN JSON: %s\n", res);
                        tdat_json_value(&json_src, &plist_dst);
                        tdat_append1(&plist_dst, "\0");

                        printf("event %zu\n%s\n", strlen(plist_dst.data), plist_dst.data);
                        fflush(stdout);

                        tdat_reset(&json_src);
                        tdat_reset(&plist_dst);
                }
        }
        tdat_drop(&json_src);
        tdat_drop(&plist_dst);
        return NULL;
}

/*
 * NOTE: Emacs sends HUP when associated buffer is killed
 * kind of graceful exit
 */
static void
on_sighup(int sig)
{
        close(0);
}

static void
stdin_loop(void* cln)
{
        struct telega_dat plist_src = TDAT_INIT;
        struct telega_dat json_dst = TDAT_INIT;
        char cmdline[33];

        signal(SIGHUP, on_sighup);
        while (fgets(cmdline, 33, stdin)) {
                cmdline[32] = '\0';

                char cmd[33];
                size_t cmdsz;
                if (2 != sscanf(cmdline, "%s %zu\n", cmd, &cmdsz)) {
                        fprintf(stderr, "Unexpected cmdline format: %s", cmdline);
                        continue;
                }

                tdat_ensure(&plist_src, cmdsz);

                /* read including newline */
                size_t rc = fread(plist_src.data, 1, cmdsz + 1, stdin);
                if (rc != cmdsz + 1) {
                        /* EOF or error */
                        fprintf(stderr, "fread() error: %d\n", ferror(stdin));
                        break;
                }
                plist_src.end = cmdsz + 1;

                tdat_plist_value(&plist_src, &json_dst);
                tdat_append1(&json_dst, "\0");

                if (!strcmp(cmd, "send"))
                        td_json_client_send(cln, json_dst.data);
                else
                        fprintf(stderr, "Unknown command: %s", cmd);

                tdat_reset(&plist_src);
                tdat_reset(&json_dst);
        }

        tdat_drop(&plist_src);
        tdat_drop(&json_dst);
}

static void
parse_stdin(void)
{
        struct telega_dat src = TDAT_INIT;

#define RDSIZE 1024
        tdat_ensure(&src, RDSIZE);

        ssize_t rlen;
        while ((rlen = read(0, &src.data[src.end], RDSIZE)) > 0) {
                src.end += rlen;
                tdat_ensure(&src, RDSIZE);
        }
#undef RDSIZE
        tdat_append1(&src, "\0");

        struct telega_dat dst = TDAT_INIT;
        if (parse_mode == PARSE_MODE_JSON)
                tdat_json_value(&src, &dst);
        else
                tdat_plist_value(&src, &dst);
        tdat_append1(&dst, "\0");

        printf("%s\n", dst.data);

        tdat_drop(&src);
        tdat_drop(&dst);
}

int
main(int ac, char** av)
{
        int ch;
        while ((ch = getopt(ac, av, "jpl:v:h")) != -1) {
                switch (ch) {
                case 'v':
                        verbosity = atoi(optarg);
                        td_set_log_verbosity_level(verbosity);
                        break;
                case 'l':
                        logfile = optarg;
                        td_set_log_file_path(logfile);
                        break;
                case 'j':
                        parse_mode = PARSE_MODE_JSON;
                        break;
                case 'p':
                        parse_mode = PARSE_MODE_PLIST;
                        break;
                case 'h':
                case '?':
                default:
                        usage(av[0]);
                }
        }
        if (parse_mode) {
                parse_stdin();
                return 0;
                /* NOT REACHED */
        }

        td_set_log_fatal_error_callback(on_error_cb);

        void *client = td_json_client_create();

        pthread_t td_thread;
        int rc = pthread_create(&td_thread, NULL, tdlib_loop, client);
        assert(rc == 0);

        stdin_loop(client);

        td_json_client_destroy(client);

        return 0;
}

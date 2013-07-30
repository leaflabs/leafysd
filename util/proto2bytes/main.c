#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>

#include "raw_packets.h"
#include "sockutil.h"
#include "proto/data.pb-c.h"

#define PROGRAM_NAME "proto2bytes"
#define DAEMON_PORT 7654

enum out_type {
    OUT_CHANNEL,
    OUT_DAC,
};

static const char* out_type_str(enum out_type type)
{
    switch (type) {
    case OUT_CHANNEL: return "channel";
    case OUT_DAC: return "dac";
    default: return "<UNKNOWN out_type>";
    }
}

static void usage(int exit_status)
{
    printf("Usage: %s [-d|-c <ch>] [-p <port>] [-s]\n"
           "Options:\n"
           "  -c, --channel"
           "\t(16-bit) channel to output\n"
           "  -d, --dac"
           "\tOutput DAC channel (this is the default)\n"
           "  -h, --help"
           "\tPrint this message\n"
           "  -p, --port"
           "\tListen to daemon at this address, default %d\n"
           "  -s, --string"
           "\tOutput values as strings on stdout instead of bytes\n"
           ,
           PROGRAM_NAME, DAEMON_PORT);
    exit(exit_status);
}

#define DEFAULT_ARGUMENTS          \
    { .daemon_port = DAEMON_PORT,  \
      .output = OUT_DAC,           \
      .channel = -1,               \
      .enable_string = 0,          \
    }

struct arguments {
    uint16_t daemon_port;
    enum out_type output;
    unsigned channel;
    int enable_string;
};

static void parse_args(struct arguments* args, int argc, char *const argv[])
{
    int print_usage = 0;
    const char shortopts[] = "c:dhp:s";
    struct option longopts[] = {
        /* Keep these sorted with shortopts. */
        { .name = "channel",
          .has_arg = required_argument,
          .flag = NULL,
          .val = 'c' },
        { .name = "dac",
          .has_arg = no_argument,
          .flag = NULL,
          .val = 'd' },
        { .name = "help",
          .has_arg = no_argument,
          .flag = NULL,
          .val = 'h' },
        { .name = "port",
          .has_arg = required_argument,
          .flag = &print_usage,
          .val = 'p' },
        { .name = "string",
          .has_arg = no_argument,
          .flag = &print_usage,
          .val = 's' },
        {0, 0, 0, 0},
    };
    /* TODO add error handling in strtol() argument conversion */
    while (1) {
        int option_idx = 0;
        int c = getopt_long(argc, argv, shortopts, longopts, &option_idx);
        if (c == -1) {
            break;
        }
        switch (c) {
        case 0:
            /* Check print_usage, which we treat as a special case. */
            if (print_usage) {
                usage(EXIT_SUCCESS);
            }
        case 'c':
            args->output = OUT_CHANNEL;
            int ch = strtol(optarg, (char**)0, 10);
            if (ch < 0) {
                fprintf(stderr, "channel %d must be positive\n", ch);
                usage(EXIT_FAILURE);
            }
            args->channel = (unsigned)ch;
            break;
        case 'd':
            args->output = OUT_DAC;
            break;
        case 'h':
            usage(EXIT_SUCCESS);
            break;
        case 'p':
            args->daemon_port = strtol(optarg, (char**)0, 10);
            break;
        case 's':
            args->enable_string = 1;
            break;
        case '?': /* Fall through. */
        default:
            usage(EXIT_FAILURE);
        }
    }
    if (args->output == OUT_CHANNEL && (args->channel > RAW_BSUB_NSAMP)) {
        fprintf(stderr, "channel %u is out of range (max is %u)\n",
                args->channel, RAW_BSUB_NSAMP);
        exit(EXIT_FAILURE);
    }
}

int main(int argc, char *argv[])
{
    uint8_t buf[1024*1024];
    struct arguments args = DEFAULT_ARGUMENTS;

    parse_args(&args, argc, argv);

    char chstr[20];
    snprintf(chstr, sizeof(chstr), "%d", args.channel);
    fprintf(stderr, "daemon port=%u, output field type=%s%s%s\n",
            args.daemon_port, out_type_str(args.output),
            args.output == OUT_CHANNEL ? ", channel=" : "",
            args.output == OUT_CHANNEL ? chstr : "");

    int sockfd = sockutil_get_udp_socket(args.daemon_port);
    if (sockfd == -1) {
        perror("sockutil_get_udp_socket");
        exit(EXIT_FAILURE);
    }

    uint32_t last_sidx = 0;
    while (1) {
        ssize_t n = recvfrom(sockfd, buf, sizeof(buf), 0, NULL, NULL);
        if (n == -1) {
            if (errno == EINTR) {
                continue;
            }
            perror("recvfrom");
            exit(EXIT_FAILURE);
        }
        DnodeSample *samp = dnode_sample__unpack(NULL, (size_t)n, buf);
        if (!samp) {
            fprintf(stderr, "unpacking failed; skipping packet\n");
            continue;
        }
        if (args.output == OUT_DAC) {
            uint8_t dac = (uint8_t)samp->subsample->dac_value;
            if (args.enable_string) {
                printf("%u\n", dac);
            } else {
                __unused ssize_t n = write(STDOUT_FILENO, &dac, sizeof(dac));
            }
        } else {
            uint16_t chan = (uint16_t)samp->subsample->samples[args.channel];
            uint8_t low = chan & 0xFF;
            if (args.enable_string) {
                printf("%u\n", chan);
            } else {
                // NB: only writing the "low" 8bits of sample for waveform
                // display
                __unused ssize_t n = write(STDOUT_FILENO, &low, sizeof(low));
            }
        }
        uint32_t gap = samp->subsample->samp_idx - last_sidx - 1;
        if (gap) {
            fprintf(stderr, "GAP: %d\n", gap);
        }
        last_sidx = samp->subsample->samp_idx;
        dnode_sample__free_unpacked(samp, NULL);
    }

    exit(EXIT_SUCCESS);         /* placate compiler */
}
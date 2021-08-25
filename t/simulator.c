/*
 * Copyright (c) 2021 Fastly, Kazuho Oku
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <getopt.h>
#include <netinet/in.h>
#include <openssl/bio.h>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include "picotls.h"
#include "picotls/openssl.h"
#include "quicly.h"
#include "quicly/cc.h"
#include "quicly/defaults.h"

static double now = 1000;

static quicly_address_t new_address(void)
{
    static uint32_t next_ipaddr = 0xac100001;
    quicly_address_t addr = {};
    addr.sin.sin_family = AF_INET;
    addr.sin.sin_addr.s_addr = htonl(next_ipaddr);
    addr.sin.sin_port = htons(54321);
    ++next_ipaddr;
    return addr;
}

/**
 * Packet
 */
struct net_packet {
    /**
     * used by nodes to maintain the linked-list of packets being queued
     */
    struct net_packet *next;
    /**
     * 4-tuple
     */
    quicly_address_t dest, src;
    /**
     * used by queues to retain when the packet entered that queue
     */
    double enter_at;
    /**
     * size of the packet
     */
    size_t size;
    /**
     * the packet
     */
    uint8_t bytes[1];
};

static struct net_packet *net_packet_create(quicly_address_t *dest, quicly_address_t *src, ptls_iovec_t vec)
{
    struct net_packet *p = malloc(offsetof(struct net_packet, bytes) + vec.len);

    p->next = NULL;
    p->dest = *dest;
    p->src = *src;
    p->enter_at = now;
    p->size = vec.len;
    memcpy(p->bytes, vec.base, vec.len);

    return p;
}

static void net_packet_destroy(struct net_packet *packet)
{
    free(packet);
}

struct net_node {
    void (*forward_)(struct net_node *node, struct net_packet *packet);
    double (*next_run_at)(struct net_node *node);
    void (*run)(struct net_node *node);
};

struct net_queue {
    struct net_node super;
    struct net_node *next_node;
    struct {
        struct net_packet *first, **append_at;
    } queue;
    double next_emit_at;
    double prop_delay;
    double bytes_per_sec;
    size_t size;
    size_t capacity;
};

static void net_queue_forward(struct net_node *_node, struct net_packet *packet)
{
    struct net_queue *queue = (struct net_queue *)_node;

    /* drop the packet if the queue is full */
    if (queue->size + packet->size > queue->capacity) {
        printf("drop %f %zu\n", now, queue->size);
        net_packet_destroy(packet);
        return;
    }

    printf("enqueue %f %zu\n", now, queue->size);
    packet->enter_at = now;
    *queue->queue.append_at = packet;
    queue->queue.append_at = &packet->next;
    queue->size += packet->size;
}

static double net_queue_next_run_at(struct net_node *_node)
{
    struct net_queue *queue = (struct net_queue *)_node;

    if (queue->queue.first == NULL)
        return INFINITY;

    double emit_at = queue->queue.first->enter_at + queue->prop_delay;
    if (emit_at < queue->next_emit_at)
        emit_at = queue->next_emit_at;

    return emit_at;
}

static void net_queue_run(struct net_node *_node)
{
    struct net_queue *queue = (struct net_queue *)_node;

    if (net_queue_next_run_at(&queue->super) > now)
        return;

    /* detach packet */
    struct net_packet *packet = queue->queue.first;
    if ((queue->queue.first = packet->next) == NULL)
        queue->queue.append_at = &queue->queue.first;
    queue->size -= packet->size;

    /* update next emission timer */
    queue->next_emit_at = now + (double)packet->size / queue->bytes_per_sec;

    /* forward to the next node */
    queue->next_node->forward_(queue->next_node, packet);

    printf("shift %f %zu\n", now, queue->size);
}

static void net_queue_init(struct net_queue *queue, double prop_delay, double bytes_per_sec, double capacity_in_sec)
{
    *queue = (struct net_queue){
        .super = {net_queue_forward, net_queue_next_run_at, net_queue_run},
        .queue = {.append_at = &queue->queue.first},
        .prop_delay = prop_delay,
        .bytes_per_sec = bytes_per_sec,
        .capacity = (size_t)(bytes_per_sec * capacity_in_sec),
    };
}

struct net_endpoint {
    struct net_node super;
    quicly_address_t addr;
    struct net_node *egress;
    quicly_conn_t *quic;
    quicly_context_t *accept_ctx;
};

static quicly_cid_plaintext_t next_quic_cid;

static void net_endpoint_forward(struct net_node *_node, struct net_packet *packet)
{
    struct net_endpoint *endpoint = (struct net_endpoint *)_node;

    size_t off = 0;
    while (off != packet->size) {
        quicly_decoded_packet_t qp;
        if (quicly_decode_packet(endpoint->quic != NULL ? quicly_get_context(endpoint->quic) : endpoint->accept_ctx, &qp,
                                 packet->bytes, packet->size, &off) == SIZE_MAX)
            break;
        if (endpoint->quic == NULL) {
            if (endpoint->accept_ctx != NULL) {
                if (quicly_accept(&endpoint->quic, endpoint->accept_ctx, &packet->dest.sa, &packet->src.sa, &qp, NULL,
                                  &next_quic_cid, NULL) == 0) {
                    assert(endpoint->quic != NULL);
                    ++next_quic_cid.master_id;
                } else {
                    assert(endpoint->quic == NULL);
                }
            }
        } else {
            quicly_receive(endpoint->quic, &packet->dest.sa, &packet->src.sa, &qp);
        }
    }

    net_packet_destroy(packet);
}

static double net_endpoint_next_run_at(struct net_node *_node)
{
    struct net_endpoint *endpoint = (struct net_endpoint *)_node;

    if (endpoint->quic == NULL)
        return INFINITY;
    /* returned value is incremented by 0.1ms to avoid the timer firing earlier than specified due to rounding error */
    double at = quicly_get_first_timeout(endpoint->quic) / 1000. + 0.0001;
    if (at < now)
        at = now;
    return at;
}

static void net_endpoint_run(struct net_node *_node)
{
    struct net_endpoint *endpoint = (struct net_endpoint *)_node;
    quicly_address_t dest, src;
    struct iovec datagrams[10];
    size_t num_datagrams = PTLS_ELEMENTSOF(datagrams);
    uint8_t buf[PTLS_ELEMENTSOF(datagrams) * 1500];
    int ret;

    if ((ret = quicly_send(endpoint->quic, &dest, &src, datagrams, &num_datagrams, buf, sizeof(buf))) == 0) {
        for (size_t i = 0; i < num_datagrams; ++i) {
            struct net_packet *packet =
                net_packet_create(&dest, &src, ptls_iovec_init(datagrams[i].iov_base, datagrams[i].iov_len));
            endpoint->egress->forward_(endpoint->egress, packet);
        }
    } else if (ret == QUICLY_ERROR_FREE_CONNECTION) {
        quicly_free(endpoint->quic);
        endpoint->quic = NULL;
    }
}

static void net_endpoint_init(struct net_endpoint *endpoint)
{
    *endpoint = (struct net_endpoint){
        .super = {net_endpoint_forward, net_endpoint_next_run_at, net_endpoint_run},
        .addr = new_address(),
    };
}

static void run_nodes(struct net_node **nodes)
{
    double next_now = INFINITY;
    for (struct net_node **node = nodes; *node != NULL; ++node) {
        double at = (*node)->next_run_at(*node);
        assert(at >= now);
        if (next_now > at)
            next_now = at;
    }

    if (isinf(next_now))
        return;

    now = next_now;
    for (struct net_node **node = nodes; *node != NULL; ++node) {
        if ((*node)->next_run_at(*node) <= now)
            (*node)->run(*node);
    }
}

static uint64_t tls_now_cb(ptls_get_time_t *self)
{
    return (uint64_t)(now * 1000);
}

static int64_t quic_now_cb(quicly_now_t *self)
{
    return (int64_t)(now * 1000);
}

static void stream_destroy_cb(quicly_stream_t *stream, int err)
{
}

static void stream_egress_shift_cb(quicly_stream_t *stream, size_t delta)
{
}

static void stream_egress_emit_cb(quicly_stream_t *stream, size_t off, void *dst, size_t *len, int *wrote_all)
{
    assert(quicly_is_client(stream->conn));
    memset(dst, 'A', *len);
    *wrote_all = 0;
}

static void stream_on_stop_sending_cb(quicly_stream_t *stream, int err)
{
    assert(!"unexpected");
}

static void stream_on_receive_cb(quicly_stream_t *stream, size_t off, const void *src, size_t len)
{
    assert(!quicly_is_client(stream->conn));
    assert(!quicly_recvstate_transfer_complete(&stream->recvstate));

    if (stream->recvstate.data_off < stream->recvstate.received.ranges[0].end)
        quicly_stream_sync_recvbuf(stream, stream->recvstate.received.ranges[0].end - stream->recvstate.data_off);
}

static void stream_on_receive_reset_cb(quicly_stream_t *stream, int err)
{
    assert(!"unexpected");
}

static int stream_open_cb(quicly_stream_open_t *self, quicly_stream_t *stream)
{
    static const quicly_stream_callbacks_t stream_callbacks = {stream_destroy_cb,     stream_egress_shift_cb,
                                                               stream_egress_emit_cb, stream_on_stop_sending_cb,
                                                               stream_on_receive_cb,  stream_on_receive_reset_cb};
    stream->callbacks = &stream_callbacks;
    return 0;
}

FILE *quicly_trace_fp;

#define RSA_PRIVATE_KEY                                                                                                            \
    "-----BEGIN RSA PRIVATE KEY-----\n"                                                                                            \
    "MIIEpAIBAAKCAQEA7zZheZ4ph98JaedBNv9kqsVA9CSmhd69kBc9ZAfVFMA4VQwp\n"                                                           \
    "rOj3ZGrxf20HB3FkvqGvew9ZogUF6NjbPumeiUObGpP21Y5wcYlPL4aojlrwMB/e\n"                                                           \
    "OxOCpuRyQTRSSe1hDPvdJABQdmshDP5ZSEBLdUSgrNn4KWhIDjFj1AHXIMqeqTXe\n"                                                           \
    "tFuRgNzHdtbXQx+UWBis2B6qZJuqSArb2msVOC8D5gNznPPlQw7FbdPCaLNXSb6G\n"                                                           \
    "nI0E0uj6QmYlAw9s6nkgP/zxjfFldqPNUprGcEqTwmAb8VVtd7XbANYrzubZ4Nn6\n"                                                           \
    "/WXrCrVxWUmh/7Spgdwa/I4Nr1JHv9HHyL2z/wIDAQABAoIBAEVPf2zKrAPnVwXt\n"                                                           \
    "cJLr6xIj908GM43EXS6b3TjXoCDUFT5nOMgV9GCPMAwY3hmE/IjTtlG0v+bXB8BQ\n"                                                           \
    "3S3caQgio5VO3A1CqUfsXhpKLRqaNM/s2+pIG+oZdRV5gIJVGnK1o3yj7qxxG/F0\n"                                                           \
    "3Q+3OWXwDZIn0eTFh2M9YkxygA/KtkREZWv8Q8qZpdOpJSBYZyGE97Jqy/yGc+DQ\n"                                                           \
    "Vpoa9B8WwnIdUn47TkZfsbzqGIYZxatJQDC1j7Y+F8So7zBbUhpz7YqATQwf5Efm\n"                                                           \
    "K2xwvlwfdwykq6ffEr2M/Xna0220G2JZlGq3Cs2X9GT9Pt9OS86Bz+EL46ELo0tZ\n"                                                           \
    "yfHQe/kCgYEA+zh4k2be6fhQG+ChiG3Ue5K/kH2prqyGBus61wHnt8XZavqBevEy\n"                                                           \
    "4pdmvJ6Q1Ta9Z2YCIqqNmlTdjZ6B35lvAK8YFITGy0MVV6K5NFYVfhALWCQC2r3B\n"                                                           \
    "6uH39FQ0mDo3gS5ZjYlUzbu67LGFnyX+pyMr2oxlhI1fCY3VchXQAOsCgYEA88Nt\n"                                                           \
    "CwSOaZ1fWmyNAgXEAX1Jx4XLFYgjcA/YBXW9gfQ0AfufB346y53PsgjX1lB+Bbcg\n"                                                           \
    "cY/o5W7F0b3A0R4K5LShlPCq8iB2DC+VnpKwTgo8ylh+VZCPy2BmMK0jrrmyqWeg\n"                                                           \
    "PzwgP0lp+7l/qW8LDImeYi8nWoqd6f1ye4iJdD0CgYEAlIApJljk5EFYeWIrmk3y\n"                                                           \
    "EKoKewsNRqfNAkICoh4KL2PQxaAW8emqPq9ol47T5nVZOMnf8UYINnZ8EL7l3psA\n"                                                           \
    "NtNJ1Lc4G+cnsooKGJnaUo6BZjTDSzJocsPoopE0Fdgz/zS60yOe8Y5LTKcTaaQ4\n"                                                           \
    "B+yOe74KNHSs/STOS4YBUskCgYAIqaRBZPsOo8oUs5DbRostpl8t2QJblIf13opF\n"                                                           \
    "v2ZprN0ASQngwUqjm8sav5e0BQ5Fc7mSb5POO36KMp0ckV2/vO+VFGxuyFqJmlNN\n"                                                           \
    "3Fapn1GDu1tZ/RYvGxDmn/CJsA26WXVnaeKXfStoB7KSueCBpI5dXOGgJRbxjtE3\n"                                                           \
    "tKV13QKBgQCtmLtTJPJ0Z+9n85C8kBonk2MCnD9JTYWoDQzNMYGabthzSqJqcEek\n"                                                           \
    "dvhr82XkcHM+r6+cirjdQr4Qj7/2bfZesHl5XLvoJDB1YJIXnNJOELwbktrJrXLc\n"                                                           \
    "dJ+MMvPvBAMah/tqr2DqgTGfWLDt9PJiCJVsuN2kD9toWHV08pY0Og==\n"                                                                   \
    "-----END RSA PRIVATE KEY-----\n"

#define RSA_CERTIFICATE                                                                                                            \
    "-----BEGIN CERTIFICATE-----\n"                                                                                                \
    "MIIDOjCCAiKgAwIBAgIBATANBgkqhkiG9w0BAQsFADAWMRQwEgYDVQQDEwtIMk8g\n"                                                           \
    "VGVzdCBDQTAeFw0xNDEyMTAxOTMzMDVaFw0yNDEyMDcxOTMzMDVaMBsxGTAXBgNV\n"                                                           \
    "BAMTEDEyNy4wLjAuMS54aXAuaW8wggEiMA0GCSqGSIb3DQEBAQUAA4IBDwAwggEK\n"                                                           \
    "AoIBAQDvNmF5nimH3wlp50E2/2SqxUD0JKaF3r2QFz1kB9UUwDhVDCms6PdkavF/\n"                                                           \
    "bQcHcWS+oa97D1miBQXo2Ns+6Z6JQ5sak/bVjnBxiU8vhqiOWvAwH947E4Km5HJB\n"                                                           \
    "NFJJ7WEM+90kAFB2ayEM/llIQEt1RKCs2fgpaEgOMWPUAdcgyp6pNd60W5GA3Md2\n"                                                           \
    "1tdDH5RYGKzYHqpkm6pICtvaaxU4LwPmA3Oc8+VDDsVt08Jos1dJvoacjQTS6PpC\n"                                                           \
    "ZiUDD2zqeSA//PGN8WV2o81SmsZwSpPCYBvxVW13tdsA1ivO5tng2fr9ZesKtXFZ\n"                                                           \
    "SaH/tKmB3Br8jg2vUke/0cfIvbP/AgMBAAGjgY0wgYowCQYDVR0TBAIwADAsBglg\n"                                                           \
    "hkgBhvhCAQ0EHxYdT3BlblNTTCBHZW5lcmF0ZWQgQ2VydGlmaWNhdGUwHQYDVR0O\n"                                                           \
    "BBYEFJXhddVQ68vtPvxoHWHsYkLnu3+4MDAGA1UdIwQpMCehGqQYMBYxFDASBgNV\n"                                                           \
    "BAMTC0gyTyBUZXN0IENBggkAmqS1V7DvzbYwDQYJKoZIhvcNAQELBQADggEBAJQ2\n"                                                           \
    "uvzL/lZnrsF4cvHhl/mg+s/RjHwvqFRrxOWUeWu2BQOGdd1Izqr8ZbF35pevPkXe\n"                                                           \
    "j3zQL4Nf8OxO/gx4w0165KL4dYxEW7EaxsDQUI2aXSW0JNSvK2UGugG4+E4aT+9y\n"                                                           \
    "cuBCtfWbL4/N6IMt2QW17B3DcigkreMoZavnnqRecQWkOx4nu0SmYg1g2QV4kRqT\n"                                                           \
    "nvLt29daSWjNhP3dkmLTxn19umx26/JH6rqcgokDfHHO8tlDbc9JfyxYH01ZP2Ps\n"                                                           \
    "esIiGa/LBXfKiPXxyHuNVQI+2cMmIWYf+Eu/1uNV3K55fA8806/FeklcQe/vvSCU\n"                                                           \
    "Vw6RN5S/14SQnMYWr7E=\n"                                                                                                       \
    "-----END CERTIFICATE-----\n"

int main(int argc, char **argv)
{
    ERR_load_CRYPTO_strings();
    OpenSSL_add_all_algorithms();

    ptls_iovec_t cert = {};
    {
        BIO *bio = BIO_new_mem_buf(RSA_CERTIFICATE, strlen(RSA_CERTIFICATE));
        X509 *x509 = PEM_read_bio_X509(bio, NULL, NULL, NULL);
        assert(x509 != NULL || !!"failed to load certificate");
        BIO_free(bio);
        cert.len = i2d_X509(x509, &cert.base);
        X509_free(x509);
    }

    ptls_openssl_sign_certificate_t cert_signer;
    {
        BIO *bio = BIO_new_mem_buf(RSA_PRIVATE_KEY, strlen(RSA_PRIVATE_KEY));
        EVP_PKEY *pkey = PEM_read_bio_PrivateKey(bio, NULL, NULL, NULL);
        assert(pkey != NULL || !"failed to load private key");
        BIO_free(bio);
        ptls_openssl_init_sign_certificate(&cert_signer, pkey);
        EVP_PKEY_free(pkey);
    }
    ptls_get_time_t tls_now = {tls_now_cb};
    ptls_context_t tlsctx = {.random_bytes = ptls_openssl_random_bytes,
                             .get_time = &tls_now,
                             .key_exchanges = ptls_openssl_key_exchanges,
                             .cipher_suites = ptls_openssl_cipher_suites,
                             .certificates = {&cert, 1},
                             .sign_certificate = &cert_signer.super};
    quicly_amend_ptls_context(&tlsctx);

    quicly_stream_open_t stream_open = {stream_open_cb};
    quicly_now_t quic_now = {quic_now_cb};
    quicly_context_t quicctx = quicly_spec_context;
    quicctx.now = &quic_now;
    quicctx.tls = &tlsctx;
    quicctx.stream_open = &stream_open;
    quicctx.transport_params.max_streams_uni = 10;
    quicctx.transport_params.min_ack_delay_usec = UINT64_MAX; /* disable ack-delay extension */

    /* parse args */
    double delay = 0.1, bw = 1e6, depth = 0.1;
    int ch;
    while ((ch = getopt(argc, argv, "b:c:d:q:h")) != -1) {
        switch (ch) {
        case 'b':
            if (sscanf(optarg, "%lf", &bw) != 1) {
                fprintf(stderr, "invalid bandwidth: %s\n", optarg);
                exit(1);
            }
            break;
        case 'c': {
            quicly_cc_type_t **cc;
            for (cc = quicly_cc_all_types; *cc != NULL; ++cc)
                if (strcmp((*cc)->name, optarg) == 0)
                    break;
            if (*cc != NULL) {
                quicctx.init_cc = (*cc)->cc_init;
            } else {
                fprintf(stderr, "unknown congestion controller: %s\n", optarg);
                exit(1);
            }
        } break;
        case 'd':
            if (sscanf(optarg, "%lf", &delay) != 1) {
                fprintf(stderr, "invalid delay value: %s\n", optarg);
                exit(1);
            }
            break;
        case 'q':
            if (sscanf(optarg, "%lf", &depth) != 1) {
                fprintf(stderr, "invalid queue depth: %s\n", optarg);
                exit(1);
            }
            break;
        default:
            printf("Usage: %s [-c name]\n\n", argv[0]);
            exit(0);
        }
    }

    struct net_queue bottleneck;
    struct net_endpoint server, client;

    /* init nodes */
    net_queue_init(&bottleneck, delay, bw, depth);
    net_endpoint_init(&server);
    net_endpoint_init(&client);

    /* client uploads to server through the bottleneck queue */
    client.egress = &bottleneck.super;
    bottleneck.next_node = &server.super;
    server.egress = &client.super;

    /* start */
    server.accept_ctx = &quicctx;
    int ret = quicly_connect(&client.quic, &quicctx, "hello.example.com", &server.addr.sa, &client.addr.sa, NULL,
                             ptls_iovec_init(NULL, 0), NULL, NULL);
    assert(ret == 0);
    quicly_stream_t *stream;
    ret = quicly_open_stream(client.quic, &stream, 1);
    assert(ret == 0);
    ret = quicly_stream_sync_sendbuf(stream, 1);
    assert(ret == 0);

    struct net_node *nodes[] = {&bottleneck.super, &server.super, &client.super, NULL};
    while (now < 1050)
        run_nodes(nodes);

    return 0;
}

/*-
 *   BSD LICENSE
 * 
 *   Copyright(c) 2010-2014 Intel Corporation. All rights reserved.
 *   All rights reserved.
 * 
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 * 
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 * 
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <sys/wait.h>

#include "test.h"

#ifdef RTE_LIBRTE_KNI
#include <rte_string_fns.h>
#include <rte_mempool.h>
#include <rte_ethdev.h>
#include <rte_cycles.h>
#include <rte_kni.h>

#define NB_MBUF          8192
#define MAX_PACKET_SZ    2048
#define MBUF_SZ \
	(MAX_PACKET_SZ + sizeof(struct rte_mbuf) + RTE_PKTMBUF_HEADROOM)
#define PKT_BURST_SZ     32
#define MEMPOOL_CACHE_SZ PKT_BURST_SZ
#define SOCKET           0
#define NB_RXD           128
#define NB_TXD           512
#define KNI_TIMEOUT_MS   5000 /* ms */

#define IFCONFIG      "/sbin/ifconfig "
#define TEST_KNI_PORT "test_kni_port"

/* The threshold number of mbufs to be transmitted or received. */
#define KNI_NUM_MBUF_THRESHOLD 100
static int kni_pkt_mtu = 0;

struct test_kni_stats {
	volatile uint64_t ingress;
	volatile uint64_t egress;
};

static const struct rte_eth_rxconf rx_conf = {
	.rx_thresh = {
		.pthresh = 8,
		.hthresh = 8,
		.wthresh = 4,
	},
	.rx_free_thresh = 0,
};

static const struct rte_eth_txconf tx_conf = {
	.tx_thresh = {
		.pthresh = 36,
		.hthresh = 0,
		.wthresh = 0,
	},
	.tx_free_thresh = 0,
	.tx_rs_thresh = 0,
};

static const struct rte_eth_conf port_conf = {
	.rxmode = {
		.header_split = 0,
		.hw_ip_checksum = 0,
		.hw_vlan_filter = 0,
		.jumbo_frame = 0,
		.hw_strip_crc = 0,
	},
	.txmode = {
		.mq_mode = ETH_DCB_NONE,
	},
};

static struct rte_kni_ops kni_ops = {
	.change_mtu = NULL,
	.config_network_if = NULL,
};

static unsigned lcore_master, lcore_ingress, lcore_egress;
static struct rte_kni *test_kni_ctx;
static struct test_kni_stats stats;

static volatile uint32_t test_kni_processing_flag;

static struct rte_mempool *
test_kni_create_mempool(void)
{
	struct rte_mempool * mp;

	mp = rte_mempool_lookup("kni_mempool");
	if (!mp)
		mp = rte_mempool_create("kni_mempool",
				NB_MBUF,
				MBUF_SZ,
				MEMPOOL_CACHE_SZ,
				sizeof(struct rte_pktmbuf_pool_private),
				rte_pktmbuf_pool_init,
				NULL,
				rte_pktmbuf_init,
				NULL,
				SOCKET,
				0);

	return mp;
}

static struct rte_mempool *
test_kni_lookup_mempool(void)
{
	return rte_mempool_lookup("kni_mempool");
}
/* Callback for request of changing MTU */
static int
kni_change_mtu(uint8_t port_id, unsigned new_mtu)
{
	printf("Change MTU of port %d to %u\n", port_id, new_mtu);
	kni_pkt_mtu = new_mtu;	
	printf("Change MTU of port %d to %i successfully.\n",
					 port_id, kni_pkt_mtu);
	return 0;
}
/**
 * This loop fully tests the basic functions of KNI. e.g. transmitting,
 * receiving to, from kernel space, and kernel requests.
 *
 * This is the loop to transmit/receive mbufs to/from kernel interface with
 * supported by KNI kernel module. The ingress lcore will allocate mbufs and
 * transmit them to kernel space; while the egress lcore will receive the mbufs
 * from kernel space and free them.
 * On the master lcore, several commands will be run to check handling the
 * kernel requests. And it will finally set the flag to exit the KNI
 * transmitting/receiving to/from the kernel space.
 *
 * Note: To support this testing, the KNI kernel module needs to be insmodded
 * in one of its loopback modes.
 */
static int
test_kni_loop(__rte_unused void *arg)
{
	int ret = 0;
	unsigned nb_rx, nb_tx, num, i;
	const unsigned lcore_id = rte_lcore_id();
	struct rte_mbuf *pkts_burst[PKT_BURST_SZ];

	if (lcore_id == lcore_master) {
		rte_delay_ms(KNI_TIMEOUT_MS);
		/* tests of handling kernel request */
		if (system(IFCONFIG TEST_KNI_PORT" up") == -1)
			ret = -1;
		if (system(IFCONFIG TEST_KNI_PORT" mtu 1400") == -1)
			ret = -1;
		if (system(IFCONFIG TEST_KNI_PORT" down") == -1)
			ret = -1;
		rte_delay_ms(KNI_TIMEOUT_MS);
		test_kni_processing_flag = 1;
	} else if (lcore_id == lcore_ingress) {
		struct rte_mempool *mp = test_kni_lookup_mempool();

		if (mp == NULL)
			return -1;

		while (1) {
			if (test_kni_processing_flag)
				break;

			for (nb_rx = 0; nb_rx < PKT_BURST_SZ; nb_rx++) {
				pkts_burst[nb_rx] = rte_pktmbuf_alloc(mp);
				if (!pkts_burst[nb_rx])
					break;
			}

			num = rte_kni_tx_burst(test_kni_ctx, pkts_burst,
								nb_rx);
			stats.ingress += num;
			rte_kni_handle_request(test_kni_ctx);
			if (num < nb_rx) {
				for (i = num; i < nb_rx; i++) {
					rte_pktmbuf_free(pkts_burst[i]);
				}
			}
			rte_delay_ms(10);
		}
	} else if (lcore_id == lcore_egress) {
		while (1) {
			if (test_kni_processing_flag)
				break;
			num = rte_kni_rx_burst(test_kni_ctx, pkts_burst,
							PKT_BURST_SZ);
			stats.egress += num;
			for (nb_tx = 0; nb_tx < num; nb_tx++)
				rte_pktmbuf_free(pkts_burst[nb_tx]);
			rte_delay_ms(10);
		}
	}

	return ret;
}

static int
test_kni_allocate_lcores(void)
{
	unsigned i, count = 0;

	lcore_master = rte_get_master_lcore();
	printf("master lcore: %u\n", lcore_master);
	for (i = 0; i < RTE_MAX_LCORE; i++) {
		if (count >=2 )
			break;
		if (rte_lcore_is_enabled(i) && i != lcore_master) {
			count ++;
			if (count == 1)
				lcore_ingress = i;
			else if (count == 2)
				lcore_egress = i;
		}
	}
	printf("count: %u\n", count);

	return (count == 2 ? 0 : -1);
}

static int
test_kni_register_handler_mp(void)
{
#define TEST_KNI_HANDLE_REQ_COUNT    10  /* 5s */
#define TEST_KNI_HANDLE_REQ_INTERVAL 500 /* ms */
#define TEST_KNI_MTU                 1450
#define TEST_KNI_MTU_STR             " 1450"
	int pid;

	pid = fork();
	if (pid < 0) {
		printf("Failed to fork a process\n");
		return -1;
	} else if (pid == 0) {
		int i;
		struct rte_kni *kni = rte_kni_get(TEST_KNI_PORT);
		struct rte_kni_ops ops = {
			.change_mtu = kni_change_mtu,
			.config_network_if = NULL,
		};

		if (!kni) {
			printf("Failed to get KNI named %s\n", TEST_KNI_PORT);
			exit(-1);
		}

		kni_pkt_mtu = 0;

		/* Check with the invalid parameters */
		if (rte_kni_register_handlers(kni, NULL) == 0) {
			printf("Unexpectedly register successuflly "
					"with NULL ops pointer\n");
			exit(-1);
		}
		if (rte_kni_register_handlers(NULL, &ops) == 0) {
			printf("Unexpectedly register successfully "
					"to NULL KNI device pointer\n");
			exit(-1);
		}

		if (rte_kni_register_handlers(kni, &ops)) {
			printf("Fail to register ops\n");
			exit(-1);
		}

		/* Check registering again after it has been registered */
		if (rte_kni_register_handlers(kni, &ops) == 0) {
			printf("Unexpectedly register successfully after "
					"it has already been registered\n");
			exit(-1);
		}

		/**
		 * Handle the request of setting MTU,
		 * with registered handlers.
		 */
		for (i = 0; i < TEST_KNI_HANDLE_REQ_COUNT; i++) {
			rte_kni_handle_request(kni);
			if (kni_pkt_mtu == TEST_KNI_MTU)
				break;
			rte_delay_ms(TEST_KNI_HANDLE_REQ_INTERVAL);
		}
		if (i >= TEST_KNI_HANDLE_REQ_COUNT) {
			printf("MTU has not been set\n");
			exit(-1);
		}

		kni_pkt_mtu = 0;
		if (rte_kni_unregister_handlers(kni) < 0) {
			printf("Fail to unregister ops\n");
			exit(-1);
		}

		/* Check with invalid parameter */
		if (rte_kni_unregister_handlers(NULL) == 0) {
			exit(-1);
		}

		/**
		 * Handle the request of setting MTU,
		 * without registered handlers.
		 */
		for (i = 0; i < TEST_KNI_HANDLE_REQ_COUNT; i++) {
			rte_kni_handle_request(kni);
			if (kni_pkt_mtu != 0)
				break;
			rte_delay_ms(TEST_KNI_HANDLE_REQ_INTERVAL);
		}
		if (kni_pkt_mtu != 0) {
			printf("MTU shouldn't be set\n");
			exit(-1);
		}

		exit(0);
	} else {
		int p_ret, status;

		rte_delay_ms(1000);
		if (system(IFCONFIG TEST_KNI_PORT " mtu" TEST_KNI_MTU_STR)
								== -1)
			return -1;

		rte_delay_ms(1000);
		if (system(IFCONFIG TEST_KNI_PORT " mtu" TEST_KNI_MTU_STR)
								== -1)
			return -1;

		p_ret = wait(&status);
		if (!WIFEXITED(status)) {
			printf("Child process (%d) exit abnormally\n", p_ret);
			return -1;
		}
		if (WEXITSTATUS(status) != 0) {
			printf("Child process exit with failure\n");
			return -1;
		}
	}

	return 0;
}

static int
test_kni_processing(uint8_t port_id, struct rte_mempool *mp)
{
	int ret = 0;
	unsigned i;
	struct rte_kni *kni;
	struct rte_kni_conf conf;
	struct rte_eth_dev_info info;
	struct rte_kni_ops ops;

	if (!mp)
		return -1;

	memset(&conf, 0, sizeof(conf));
	memset(&info, 0, sizeof(info));
	memset(&ops, 0, sizeof(ops));

	rte_eth_dev_info_get(port_id, &info);
	conf.addr = info.pci_dev->addr;
	conf.id = info.pci_dev->id;
	rte_snprintf(conf.name, sizeof(conf.name), TEST_KNI_PORT);

	/* core id 1 configured for kernel thread */
	conf.core_id = 1;
	conf.force_bind = 1;
	conf.mbuf_size = MAX_PACKET_SZ;
	conf.group_id = (uint16_t)port_id;

	ops = kni_ops;
	ops.port_id = port_id;

	/* basic test of kni processing */
	kni = rte_kni_alloc(mp, &conf, &ops);
	if (!kni) {
		printf("fail to create kni\n");
		return -1;
	}
	if (rte_kni_get_port_id(kni) != port_id) {
		printf("fail to get port id\n");
		ret = -1;
		goto fail_kni;
	}

	if (rte_kni_info_get(RTE_MAX_ETHPORTS)) {
		printf("Unexpectedly get a KNI successfully\n");
		ret = -1;
		goto fail_kni;
	}

	test_kni_ctx = kni;
	test_kni_processing_flag = 0;
	stats.ingress = 0;
	stats.egress = 0;

	/**
	 * Check multiple processes support on
	 * registerring/unregisterring handlers.
	 */
	if (test_kni_register_handler_mp() < 0) {
		printf("fail to check multiple process support\n");
		ret = -1;
		goto fail_kni;
	}

	rte_eal_mp_remote_launch(test_kni_loop, NULL, CALL_MASTER);
	RTE_LCORE_FOREACH_SLAVE(i) {
		if (rte_eal_wait_lcore(i) < 0) {
			ret = -1;
			goto fail_kni;
		}
	}
	/**
	 * Check if the number of mbufs received from kernel space is equal
	 * to that of transmitted to kernel space
	 */
	if (stats.ingress < KNI_NUM_MBUF_THRESHOLD ||
		stats.egress < KNI_NUM_MBUF_THRESHOLD) {
		printf("The ingress/egress number should not be "
			"less than %u\n", (unsigned)KNI_NUM_MBUF_THRESHOLD);
		ret = -1;
		goto fail_kni;
	}

	if (rte_kni_release(kni) < 0) {
		printf("fail to release kni\n");
		return -1;
	}
	test_kni_ctx = NULL;
	
	/* test of releasing a released kni device */
	if (rte_kni_release(kni) == 0) {
		printf("should not release a released kni device\n");
		return -1;
	}

	/* test of reusing memzone */
	kni = rte_kni_alloc(mp, &conf, &ops);
	if (!kni) {
		printf("fail to create kni\n");
		return -1;
	}

	/* Release the kni for following testing */
	if (rte_kni_release(kni) < 0) {
		printf("fail to release kni\n");
		return -1;
	}
	
	return ret;
fail_kni:
	if (rte_kni_release(kni) < 0) {
		printf("fail to release kni\n");
		ret = -1;
	}

	return ret;
}

int
test_kni(void)
{
	int ret = -1;
	uint8_t nb_ports, port_id;
	struct rte_kni *kni;
	struct rte_mempool *mp;
	struct rte_kni_conf conf;
	struct rte_eth_dev_info info;
	struct rte_kni_ops ops;

	if (test_kni_allocate_lcores() < 0) {
		printf("No enough lcores for kni processing\n");
		return -1;
	}

	mp = test_kni_create_mempool();
	if (!mp) {
		printf("fail to create mempool for kni\n");
		return -1;
	}
	ret = rte_pmd_init_all();
	if (ret < 0) {
		printf("fail to initialize PMD\n");
		return -1;
	}
	ret = rte_eal_pci_probe();
	if (ret < 0) {
		printf("fail to probe PCI devices\n");
		return -1;
	}

	nb_ports = rte_eth_dev_count();
	if (nb_ports == 0) {
		printf("no supported nic port found\n");
		return -1;
	}

	/* configuring port 0 for the test is enough */
	port_id = 0;
	ret = rte_eth_dev_configure(port_id, 1, 1, &port_conf);
	if (ret < 0) {
		printf("fail to configure port %d\n", port_id);
		return -1;
	}

	ret = rte_eth_rx_queue_setup(port_id, 0, NB_RXD, SOCKET, &rx_conf, mp);
	if (ret < 0) {
		printf("fail to setup rx queue for port %d\n", port_id);
		return -1;
	}

	ret = rte_eth_tx_queue_setup(port_id, 0, NB_TXD, SOCKET, &tx_conf);
	if (ret < 0) {
		printf("fail to setup tx queue for port %d\n", port_id);
		return -1;
	}

	ret = rte_eth_dev_start(port_id);
	if (ret < 0) {
		printf("fail to start port %d\n", port_id);
		return -1;
	}
	rte_eth_promiscuous_enable(port_id);

	/* basic test of kni processing */
	ret = test_kni_processing(port_id, mp);
	if (ret < 0)
		goto fail;

	/* test of allocating KNI with NULL mempool pointer */
	memset(&info, 0, sizeof(info));
	memset(&conf, 0, sizeof(conf));
	memset(&ops, 0, sizeof(ops));
	rte_eth_dev_info_get(port_id, &info);
	conf.addr = info.pci_dev->addr;
	conf.id = info.pci_dev->id;
	conf.group_id = (uint16_t)port_id;
	conf.mbuf_size = MAX_PACKET_SZ;

	ops = kni_ops;
	ops.port_id = port_id;
	kni = rte_kni_alloc(NULL, &conf, &ops);
	if (kni) {
		ret = -1;
		printf("unexpectedly creates kni successfully with NULL "
							"mempool pointer\n");
		goto fail;
	}

	/* test of allocating KNI without configurations */
	kni = rte_kni_alloc(mp, NULL, NULL);
	if (kni) {
		ret = -1;
		printf("Unexpectedly allocate KNI device successfully "
					"without configurations\n");
		goto fail;
	}

	/* test of allocating KNI without a name */
	memset(&conf, 0, sizeof(conf));
	memset(&info, 0, sizeof(info));
	memset(&ops, 0, sizeof(ops));
	rte_eth_dev_info_get(port_id, &info);
	conf.addr = info.pci_dev->addr;
	conf.id = info.pci_dev->id;
	conf.group_id = (uint16_t)port_id;
	conf.mbuf_size = MAX_PACKET_SZ;

	ops = kni_ops;
	ops.port_id = port_id;
	kni = rte_kni_alloc(mp, &conf, &ops);
	if (kni) {
		ret = -1;
		printf("Unexpectedly allocate a KNI device successfully "
						"without a name\n");
		goto fail;
	}

	/* test of getting port id according to NULL kni context */
	if (rte_kni_get_port_id(NULL) < RTE_MAX_ETHPORTS) {
		ret = -1;
		printf("unexpectedly get port id successfully by NULL kni "
								"pointer\n");
		goto fail;
	}

	/* test of releasing NULL kni context */
	ret = rte_kni_release(NULL);
	if (ret == 0) {
		ret = -1;
		printf("unexpectedly release kni successfully\n");
		goto fail;
	}

	/* test of handling request on NULL device pointer */
	ret = rte_kni_handle_request(NULL);
	if (ret == 0) {
		ret = -1;
		printf("Unexpectedly handle request on NULL device pointer\n");
		goto fail;
	}

	/* test of getting KNI device with pointer to NULL */
	kni = rte_kni_get(NULL);
	if (kni) {
		ret = -1;
		printf("Unexpectedly get a KNI device with "
					"NULL name pointer\n");
		goto fail;
	}

	/* test of getting KNI device with an zero length name string */
	memset(&conf, 0, sizeof(conf));
	kni = rte_kni_get(conf.name);
	if (kni) {
		ret = -1;
		printf("Unexpectedly get a KNI device with "
				"zero length name string\n");
		goto fail;
	}

	/* test of getting KNI device with an invalid string name */
	memset(&conf, 0, sizeof(conf));
	rte_snprintf(conf.name, sizeof(conf.name), "testing");
	kni = rte_kni_get(conf.name);
	if (kni) {
		ret = -1;
		printf("Unexpectedly get a KNI device with "
				"a never used name string\n");
		goto fail;
	}

	/* test the interface of creating a KNI, for backward compatibility */
	memset(&ops, 0, sizeof(ops));
	ops = kni_ops;
	kni = rte_kni_create(port_id, MAX_PACKET_SZ, mp, &ops);
	if (!kni) {
		ret = -1;
		printf("Fail to create a KNI device for port %d\n", port_id);
		goto fail;
	}

	ret = rte_kni_release(kni);
	if (ret < 0) {
		printf("Fail to release a KNI device\n");
		goto fail;
	}

	ret = 0;

fail:
	rte_eth_dev_stop(port_id);

	return ret;
}

#else /* RTE_LIBRTE_KNI */

int
test_kni(void)
{
	printf("The KNI library is not included in this build\n");
	return 0;
}

#endif /* RTE_LIBRTE_KNI */

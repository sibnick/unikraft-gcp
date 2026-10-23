#include <stdio.h>
#include <uk/alloc.h>
#include <uk/netdev.h>
#include <uk/netbuf.h>

static uint16_t rx_pkts_alloc(void *argp, struct uk_netbuf *pkts[], uint16_t count)
{
	struct uk_alloc *a = (struct uk_alloc *)argp;
	uint16_t i;

	for (i = 0; i < count; i++) {
		pkts[i] = uk_netbuf_alloc_buf(a, 2048, 2048, 64, 0, NULL);
		if (!pkts[i])
			break;
		pkts[i]->len = 2048 - 64;
	}
	return i;
}

int main(int argc, char *argv[])
{
	struct uk_netdev *dev;
	const char *drv_name;
	const struct uk_hwaddr *hwaddr;
	struct uk_alloc *a;
	struct uk_netdev_conf conf;
	struct uk_netdev_rxqueue_conf rx_conf;
	struct uk_netdev_txqueue_conf tx_conf;
	unsigned int count;
	int rc;

	(void)argc;
	(void)argv;

	printf("\n========================================\n");
	printf("Hello from Unikraft on Google Cloud (Phase A2)!\n");
	printf("========================================\n");

	count = uk_netdev_count();
	printf("Probing network devices: count=%u\n", count);
	if (count == 0) {
		printf("GATE_FAIL: no netdev devices found\n");
		return 1;
	}

	dev = uk_netdev_get(0);
	if (!dev) {
		printf("GATE_FAIL: uk_netdev_get(0) returned NULL\n");
		return 1;
	}

	drv_name = uk_netdev_drv_name_get(dev);
	printf("Device 0 driver: %s\n", drv_name ? drv_name : "unknown");

	rc = uk_netdev_probe(dev);
	if (rc < 0) {
		printf("GATE_FAIL: uk_netdev_probe failed: %d\n", rc);
		return 1;
	}
	printf("Device 0 probed successfully\n");

	conf.nb_rx_queues = 1;
	conf.nb_tx_queues = 1;
	conf.lro = 0;

	rc = uk_netdev_configure(dev, &conf);
	if (rc < 0) {
		printf("GATE_FAIL: uk_netdev_configure failed: %d\n", rc);
		return 1;
	}
	printf("Device 0 configured successfully\n");

	hwaddr = uk_netdev_hwaddr_get(dev);
	if (hwaddr) {
		printf("Device 0 MAC address: %02x:%02x:%02x:%02x:%02x:%02x\n",
		       hwaddr->addr_bytes[0], hwaddr->addr_bytes[1],
		       hwaddr->addr_bytes[2], hwaddr->addr_bytes[3],
		       hwaddr->addr_bytes[4], hwaddr->addr_bytes[5]);
	}

	a = uk_alloc_get_default();
	if (!a) {
		printf("GATE_FAIL: uk_alloc_get_default returned NULL\n");
		return 1;
	}

	rx_conf.a = a;
	rx_conf.alloc_rxpkts = rx_pkts_alloc;
	rx_conf.alloc_rxpkts_argp = a;
	rx_conf.callback = NULL;
	rx_conf.callback_cookie = NULL;

	rc = uk_netdev_rxq_configure(dev, 0, 0, &rx_conf);
	if (rc < 0) {
		printf("GATE_FAIL: uk_netdev_rxq_configure failed: %d\n", rc);
		return 1;
	}
	printf("Device 0 RX queue configured successfully\n");

	tx_conf.a = a;

	rc = uk_netdev_txq_configure(dev, 0, 0, &tx_conf);
	if (rc < 0) {
		printf("GATE_FAIL: uk_netdev_txq_configure failed: %d\n", rc);
		return 1;
	}
	printf("Device 0 TX queue configured successfully\n");

	rc = uk_netdev_start(dev);
	if (rc < 0) {
		printf("GATE_FAIL: uk_netdev_start failed: %d\n", rc);
		return 1;
	}
	printf("Device 0 started successfully\n");

	printf("========================================\n");
	printf("GATE_VIRTIO_PASS\n");
	printf("========================================\n\n");

	return 0;
}

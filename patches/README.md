# TuyaOpen SDK patches

BLE Agent Monitor requires small changes to upstream [TuyaOpen](https://github.com/tuya/TuyaOpen). Apply from a TuyaOpen checkout root:

```bash
cd /path/to/TuyaOpen
git apply /path/to/tuya_t5_pixel_ble/patches/tuyaopen-ble-agent.patch
```

Files touched:

- `src/tuya_cloud_service/ble/ble_mgr.c` / `ble_mgr.h` — custom adv name, keep connection, agent hooks
- `src/tal_bluetooth/nimble/tkl_bluetooth.c` — `S:`/`P:`/`L:`/`I:` line passthrough, MTU fix

After applying patches, place this repo under `apps/tuya_t5_pixel/tuya_t5_pixel_demo_ble/` (or merge paths as needed).

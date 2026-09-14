This is MemProcFS plugin for reading Hyper-V memory using Hyper-V Memory Manager library

Sources was taken from https://github.com/ufrisk/LeechCore

- MemProcFs can be found on https://github.com/ufrisk/MemProcFS by @ulfrisk

- LiveCloudKd: https://github.com/gerhart01/LiveCloudKd

Copy leechcore_device_hvmm.dll with hvlib.dll and hvmm.sys to MemProcFS folder

start MemProcFS:
```
MemProcFS.exe -device hvmm -v
```

you must see something like that:

![](./images/1.png)

Next you can go to M: driver and use pypykatz plugin, f.e.

![](./images/2.png)

## `mapped`: reads through a persistent mapping

`hvmm://id=1,mapped` asks `hvmm.sys` to map the guest's physical memory into the
MemProcFS process once, read-only, and serves every scatter read from that mapping
instead of one driver round trip per page. Needs an `hvmm.sys` with
`IOCTL_MAP_GPA_RANGE`; older drivers answer the handshake with an error and the plugin
falls back to the driver path on its own. Full VMs only; containers keep the driver
path. Set `LC_HVMM_MAPPED=1` in the environment to turn it on for a program whose
device string you cannot change. If the VM stops, the next failed read drops the
mapping and the driver path takes over.

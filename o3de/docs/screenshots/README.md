# o3de PoC -- beta1 demo screenshots

Visual evidence of the M4 (zero-copy Vulkan -> Vulkan interop) milestone,
captured on `:1` window-scoped (X11 window scoped, never root) when the
PoC was tagged `o3de-poc-beta1`.

| File                                | Description                                       |
|-------------------------------------|---------------------------------------------------|
| `m4-beta1-live-demo.png`            | Single screenshot of the gz-gui MainWindow viewport while the live demo is running. Box / sphere / cylinder / wirebox / orange wall / ground grid all visible. 56 distinct quantised colours in the centre crop. |
| `m4-beta1-live-demo-3s-diff.png`    | `compare -metric AE -fuzz 3%` of two screenshots taken 3 s apart. 167 212 pixels changed -- visible motion (box spin, sphere bob, wirebox tracking the sphere, cylinder orbiting the origin) proves the on-screen viewport is genuinely live, not a single captured frame. |

## How they were captured

The harness used to produce these is in `/tmp/o3de_debug/verify_anim.sh`
on the reference machine -- kill any stale `gz gui`, launch
`live_demo.sh` fresh, wait 18 s for the QSG warm-up + the
swapchain-dump-layer WSI prime (see live_demo.sh comment), grab
`-window <wid>` via ImageMagick `import`, repeat after 3 s, diff.

Both screenshots assume the workarounds documented in `live_demo.sh`
are in place (QT_SCALE_FACTOR=1 for the Qt 6.4.2 HiDPI render-target
bug, and the swapchain-dump-layer-as-WSI-primer for the NVIDIA Vulkan
present race). Upgrading Qt to >= 6.8 may make both unnecessary.

Live SDR support (optional) / 实时 SDR 支持（可选）
====================================================

The program can read a live stream from a HackRF or RTL-SDR, but the SDR libraries are not part of
this package. They are loaded at run time, only when a live source is selected. Install them and their
driver yourself, under their own licences:

  HackRF:    hackrf.dll, libusb-1.0.dll, pthreadVC3.dll  (GPLv2, Great Scott Gadgets)
  RTL-SDR:   rtlsdr.dll, libusb-1.0.dll                 (GPLv2, Osmocom / RTL-SDR contributors)
  libusb:    libusb-1.0.dll                             (LGPL-2.1)

Put the DLLs next to this program (the folder containing NadirStation_GK2A-HRIT_decoder.exe), or
add their folder to PATH. Install the vendor driver (WinUSB via Zadig for an RTL-SDR; the HackRF
driver package for a HackRF), then choose "HackRF" or "RTL-SDR" in the signal-source list.

Recommended settings for the GK-2A HRIT downlink:

  Centre frequency : 1695.4 MHz
  Sample rate      : HackRF 6 MS/s (4 MS/s also works); RTL-SDR 3.2 MS/s
  Gain             : start with HackRF LNA 32 dB / VGA 40 dB, or RTL-SDR AGC
  Antenna          : an external 1.7 GHz LNA and band-pass filter are strongly recommended

The RTL-SDR's 3.2 MS/s is only just above the 3.15 MS/s the 3 Msym/s link needs. It can work, but
expect dropped samples and low 1.7 GHz sensitivity. The program is receive-only and never transmits.

If the device list is empty, the library or its dependency is missing, or the driver is not installed.
The log reports which library could not be loaded. HackRF streaming errors usually mean the USB port
or cable cannot deliver enough power/data; try a different USB 2.0 port and a short cable.

本程序可实时接收 HackRF / RTL-SDR，但 SDR 运行库不在本安装包内，只在选择实时源时从程序目录或
PATH 加载。请自行安装运行库和驱动，并遵守其许可证（HackRF 与 RTL-SDR 为 GPLv2，libusb 为 LGPL-2.1）。
将 DLL 放在本程序目录，或把目录加入 PATH；用 Zadig 给 RTL-SDR 安装 WinUSB 驱动，HackRF 安装官方驱动。
推荐中心频率 1695.4 MHz，HackRF 6 MS/s、RTL-SDR 3.2 MS/s，外加 1.7 GHz LNA 与带通滤波器。
程序只接收，不发射。

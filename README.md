# ECG + PCG + SD + Web

## Mục tiêu đồng bộ

- PCG: `16 000 Hz` từ ICS-43434 qua I2S RX + DMA, Core 0.
- ECG: `400 Hz` từ AD8232 OUTPUT qua `ADC1_CH0/GPIO36`, Core 1.
- `LO+ -> GPIO16`, `LO- -> GPIO4`.
- Một ECG slot luôn gắn với đúng `40` mẫu PCG:

```text
ECG[k] <-> PCG[40*k]
```

Core 0 phát một task notification ngay trước khi đợi I2S lấy một slot 40 mẫu. Core 1 thức dậy và đọc chính xác một ECG sample. Tỷ lệ 40:1 được kiểm tra bằng các bất biến biên dịch trong `app_config.h`.

## Vì sao v6 tránh lỗi v5

1. **Không tạo CSV khi đang ghi tín hiệu.** Core 1 chỉ ghi raw binary compact.
2. **Raw files được preallocate trước khi bật I2S.** FAT cluster allocation xảy ra trước record, không nằm trong pipeline realtime.
3. **Không `fflush()` có chủ ý trong capture.** Chỉ flush sau khi I2S dừng.
4. **PCG writer có ưu tiên dequeue trước ECG.** Frame PCG được trả về `free pool` nhanh nhất.
5. **Conversion BIN -> CSV chạy trong task riêng 12 KB.** Buffers converter là static; app_main không chứa mảng 800 mẫu trên stack, do đó loại bỏ lỗi stack overflow từng xuất hiện.
6. **Runtime diagnostics:** log high-water stack của PCG, ECG, SD writer và postprocess; log PASS/FAIL cuối phiên.

## Chân phần cứng

```text
ICS-43434: BCLK GPIO26, WS GPIO25, SD GPIO34, L/R GND
AD8232:    OUTPUT GPIO36, LO+ GPIO16, LO- GPIO4
SD SPI:    MOSI GPIO13, MISO GPIO12, CLK GPIO14, CS GPIO15
```

Tất cả module dùng GND chung và logic 3.3 V.

## Build

```powershell
idf.py set-target esp32
idf.py fullclean
idf.py build flash monitor
```

Trong menuconfig, kiểm tra CPU 240 MHz và FreeRTOS tick 1000 Hz. `sdkconfig.defaults` đã khai báo hai thông số này; `fullclean` giúp áp dụng sạch.

## Kết quả pass bắt buộc ở phiên 2 giây

```text
Capture check: PASS
PCG frames=40/40 drop=0 short=0
ECG=800/800 drop=0 late=0
SD errors=0
CSV conversion complete ... raw_gaps=0
```

Nếu `drop`, `short`, `late`, `sd_errors` hoặc `raw_gaps` khác 0, session phải được xem là có cờ mất dữ liệu. CSV vẫn được giữ để debug, nhưng không dùng để đánh giá tín hiệu y sinh.

Sau postprocess, ESP32 bật SoftAP:

```text
SSID: ECG-PCG-ESP32
Password: 12345678
URL: http://192.168.4.1
```

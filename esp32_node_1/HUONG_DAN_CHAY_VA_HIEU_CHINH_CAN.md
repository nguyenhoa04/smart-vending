# Hướng dẫn chạy firmware và hiệu chỉnh cân

Tài liệu này áp dụng cho mã nguồn hiện tại của dự án ESP32 Node, sử dụng ESP-IDF 5.3.1, hai HX711 có đường clock độc lập và giao tiếp với Raspberry Pi qua UART.

## 1. Kiến trúc đang được sử dụng

Firmware được chia thành các software component:

- `main/esp32_node.c`: entry point, chỉ gọi `node_app_start()`.
- `components/node_app`: khởi tạo UART, cân, khóa và khởi chạy các task.
- `components/shelf_service`: đọc HX711, lọc trọng lượng, tare, calibrate và kiểm tra bốn góc.
- `components/command_service`: nhận và xử lý lệnh từ UART hoặc console.
- `components/door_session_service`: đọc MC-38, quản lý relay và vòng đời phiên mua hàng.
- `components/hx711`: driver giao tiếp với HX711; mỗi shelf sử dụng một SCK riêng.
- `components/pi_uart`: driver UART giao tiếp với Raspberry Pi.
- `components/lock`: điều khiển relay khóa và đọc cảm biến cửa.

Firmware tạo bốn FreeRTOS task:

| Tên task | Chức năng | Chu kỳ/thời gian chờ |
|---|---|---:|
| `main_sensor_task` | Đọc và xử lý trọng lượng của hai shelf | Delay 20 ms sau mỗi vòng |
| `DOOR_SESSION` | Poll MC-38 và chạy state machine của phiên mua hàng | 20 ms |
| `LOCK` | Nhận lệnh từ UART1 | Read timeout 20 ms, sau đó delay 20 ms |
| `CONSOLE` | Nhận lệnh nhập từ ESP-IDF Monitor | Delay 10 ms khi không có dữ liệu |

## 2. Cấu hình phần cứng hiện tại

### 2.1. HX711

Hai HX711 sử dụng hai đường clock độc lập:

| Tín hiệu | GPIO ESP32 | Chức năng |
|---|---:|---|
| SCK shelf 1 | GPIO25 | Clock của HX711 thứ nhất |
| DOUT shelf 1 | GPIO34 | Dữ liệu HX711 thứ nhất |
| SCK shelf 2 | GPIO26 | Clock của HX711 thứ hai |
| DOUT shelf 2 | GPIO35 | Dữ liệu HX711 thứ hai |

Lưu ý quan trọng:

- GPIO34 và GPIO35 là các chân input-only và không có điện trở kéo nội trên ESP32 classic.
- Hai HX711 được khởi tạo và đọc độc lập. Một module chưa cấp nguồn hoặc bị timeout không chặn việc đọc module còn lại.
- Các bo mạch phải dùng chung GND với ESP32.

### 2.2. UART với Raspberry Pi

UART1 được cấu hình `115200 baud`, `8 data bits`, `no parity`, `1 stop bit`:

| ESP32 | Raspberry Pi |
|---|---|
| TX GPIO17 | RX của Pi |
| RX GPIO16 | TX của Pi |
| GND | GND |

Không nối TX với TX hoặc RX với RX. ESP32 và Raspberry Pi sử dụng UART logic 3.3 V; không đưa tín hiệu UART 5 V trực tiếp vào ESP32.

### 2.3. Khóa và cảm biến cửa

| Thiết bị | GPIO |
|---|---:|
| Relay khóa | GPIO4 |
| Cảm biến cửa MC-38 | GPIO5 |

MC-38 dùng pull-up nội và được hiểu như sau:

- GPIO5 mức thấp: cửa đóng.
- GPIO5 mức cao: cửa mở.
- Trạng thái phải ổn định qua ba lần đọc liên tiếp, tương đương khoảng 60 ms, trước khi được công nhận.

Relay chỉ được mở khóa tối đa 5 giây. Nếu người dùng đã mở cửa, session vẫn tiếp tục sau khi relay tự khóa và chỉ kết thúc khi MC-38 xác nhận cửa đã đóng lại.

## 3. Chuẩn bị môi trường ESP-IDF

Khuyến nghị mở **ESP-IDF PowerShell** do Espressif cài đặt. Nếu dùng PowerShell thông thường trên máy phát triển hiện tại, nạp môi trường bằng:

```powershell
. 'D:\Framework\esp\Espressif\frameworks\esp-idf-v5.3.1\export.ps1'
```

Đi tới thư mục dự án:

```powershell
Set-Location 'D:\Framework\esp\Espressif\frameworks\esp-idf-v5.3.1\examples\get-started\esp32_node_1'
```

Kiểm tra công cụ:

```powershell
idf.py --version
```

Target của dự án là ESP32. Chỉ cần chạy lệnh sau khi tạo build mới hoặc khi target đang bị đặt sai:

```powershell
idf.py set-target esp32
```

## 4. Build, flash và monitor

### 4.1. Build firmware

```powershell
idf.py build
```

Build thành công sẽ tạo file:

```text
build/esp32_node.bin
```

### 4.2. Xác định cổng COM

Trong PowerShell có thể kiểm tra các cổng serial bằng:

```powershell
[System.IO.Ports.SerialPort]::GetPortNames()
```

### 4.3. Flash và mở monitor

Thay `COM5` bằng cổng thực tế của ESP32:

```powershell
idf.py -p COM5 flash monitor
```

Hoặc thực hiện riêng từng bước:

```powershell
idf.py -p COM5 flash
idf.py -p COM5 monitor
```

Nhấn `Ctrl+]` để thoát ESP-IDF Monitor.

## 5. Hoạt động khi khởi động

Sau khi ESP32 reset, firmware thực hiện tuần tự:

1. Khởi tạo UART1.
2. Tạo mutex bảo vệ dữ liệu và thao tác HX711.
3. Khởi tạo hai HX711 độc lập: shelf 1 dùng SCK GPIO25, shelf 2 dùng SCK GPIO26.
4. Chờ HX711 warm-up trong 2 giây.
5. Đọc và bỏ 10 bộ mẫu đầu tiên.
6. Tự động tare `SHELF_1` và `SHELF_2`, mỗi shelf dùng tối đa 20 mẫu hợp lệ.
7. Khởi tạo relay ở trạng thái tắt và cấu hình MC-38.
8. Khởi chạy sensor task, door-session task, UART task và console task.

Vì firmware tự động tare cả hai shelf lúc khởi động, cả hai mặt cân phải hoàn toàn trống trong quá trình reset và trong vài giây đầu tiên.

## 6. Gửi lệnh cho ESP32

Có hai cách gửi lệnh:

- Nhập lệnh trực tiếp trong ESP-IDF Monitor rồi nhấn Enter.
- Gửi một chuỗi lệnh từ Raspberry Pi qua UART1.

Nên gửi từng lệnh riêng biệt, kết thúc bằng `\n`, và chờ phản hồi trước khi gửi lệnh tiếp theo. Bộ nhận UART loại bỏ khoảng trắng và ký tự xuống dòng ở hai đầu lệnh, nhưng chưa ghép các packet bị chia nhỏ.

### 6.1. Danh sách lệnh

| Lệnh | Chức năng | Phản hồi điển hình |
|---|---|---|
| `PING` | Kiểm tra kết nối | `READY` |
| `STATUS` | Đọc trạng thái hiện tại | `STATUS: door=closed;lock=locked;session=idle;weight=...` |
| `UNLOCK` | Mở khóa và chuyển sang chờ người dùng mở cửa | `LOCK: unlocked` |
| `TARE` | Đặt offset hiện tại của shelf 1 làm mốc 0 | `TARE: SHELF_1 OK` |
| `TARE:SHELF_1` | Tare riêng shelf 1 | `TARE: SHELF_1 OK` |
| `TARE:SHELF_2` | Tare riêng shelf 2 | `TARE: SHELF_2 OK` |
| `CALIBRATE:500` | Hiệu chỉnh shelf 1 bằng quả cân chuẩn 500 g, tương thích lệnh cũ | `CALIBRATE: SHELF_1 OK ...` |
| `CALIBRATE:SHELF_1:500` | Hiệu chỉnh riêng shelf 1 bằng 500 g | `CALIBRATE: SHELF_1 OK weight=500.0g scale=... raw_avg=...` |
| `CALIBRATE:SHELF_2:500` | Hiệu chỉnh riêng shelf 2 bằng 500 g | `CALIBRATE: SHELF_2 OK weight=500.0g scale=... raw_avg=...` |
| `CORNER1` | Đo góc 1 bằng trung bình 15 mẫu | `CORNER1: ... g` |
| `CORNER2` | Đo góc 2 bằng trung bình 15 mẫu | `CORNER2: ... g` |
| `CORNER3` | Đo góc 3 bằng trung bình 15 mẫu | `CORNER3: ... g` |
| `CORNER4` | Đo góc 4 bằng trung bình 15 mẫu | `CORNER4: ... g` |
| `CORNERCHECK` | Tính độ lệch của bốn góc | `CORNERCHECK: avg=... C1=... C2=...` |

Các lệnh không ghi rõ kệ (`TARE`, `CALIBRATE:<gram>`) vẫn thao tác với `SHELF_1`. Các lệnh `CORNER1..4` và `CORNERCHECK` hiện chỉ kiểm tra `SHELF_1`.

### 6.2. Vòng đời một session mua hàng

State machine của cửa có ba trạng thái:

| Trạng thái | Ý nghĩa |
|---|---|
| `idle` | Không có phiên mua hàng đang hoạt động |
| `waiting_for_open` | Đã nhận `UNLOCK`, đang chờ cửa mở |
| `active` | Cửa đã được mở và phiên mua hàng đang diễn ra |

Luồng bình thường:

1. Raspberry Pi gửi `UNLOCK`.
2. ESP32 bật relay và trả `LOCK: unlocked`.
3. Khi MC-38 xác nhận cửa mở, ESP32 gửi `DOOR: opened` và `SESSION: started`.
4. Sau tối đa 5 giây tính từ `UNLOCK`, ESP32 tắt relay và gửi `LOCK: locked`. Session vẫn là `active` nếu cửa chưa đóng.
5. Khi MC-38 xác nhận cửa đóng, ESP32 gửi `DOOR: closed` và `SESSION: ended`.
6. Hệ thống trở về `session=idle` và sẵn sàng cho phiên tiếp theo.

Nếu cửa không được mở trong vòng 5 giây, ESP32 tắt relay, gửi `LOCK: locked` và trở về `idle`; không phát `SESSION: started` hoặc `SESSION: ended`.

Nếu cửa bị mở hoặc đóng khi không có lệnh `UNLOCK`, ESP32 vẫn gửi `DOOR: opened` hoặc `DOOR: closed`, nhưng không bắt đầu một session mua hàng.

Có thể gửi `STATUS` bất kỳ lúc nào. Ví dụ khi người dùng đang mở cửa nhưng relay đã hết timeout:

```text
STATUS: door=open;lock=locked;session=active;weight=500.0
```

### 6.3. Dữ liệu trọng lượng tự động

Sensor task gửi dữ liệu theo định dạng:

```text
SHELF_1: TOTAL=500.0
SHELF_2: TOTAL=250.0
```

Dữ liệu chỉ được gửi lại khi trọng lượng đã xử lý khác lần gửi trước hơn `0.1 g`. Giá trị hiện được làm tròn theo đơn vị gram trước khi lưu vào `total_weight`.

## 7. Nguyên lý hiệu chỉnh

Firmware quy đổi raw ADC sang gram theo công thức:

```text
weight_g = (raw_value - offset) / scale
```

Trong đó:

- `offset`: giá trị raw khi mặt cân không có tải, được xác định bằng `TARE`.
- `scale`: số đơn vị raw tương ứng với một gram, được xác định bằng `CALIBRATE:<gram>`.

Khi hiệu chỉnh với quả cân chuẩn, firmware tính:

```text
new_scale = (raw_average - offset) / known_weight_g
```

## 8. Quy trình hiệu chỉnh cân đầy đủ

### Bước 1: Kiểm tra cơ khí

- Mặt bàn cân phải cứng, không chạm vào khung hoặc vật thể xung quanh.
- Bốn load cell phải được gá chắc chắn và chịu lực đúng hướng.
- Dây tín hiệu không được lỏng hoặc chịu lực kéo.
- Nếu dùng junction box, xác định đúng trim potentiometer tương ứng với từng góc.
- Nên cấp nguồn cho hệ thống vài phút trước khi hiệu chỉnh để tín hiệu ổn định.

### Bước 2: Tare mặt cân

1. Lấy toàn bộ vật ra khỏi mặt cân.
2. Không chạm hoặc tì lên mặt cân.
3. Gửi:

```text
TARE
```

4. Chờ phản hồi:

```text
TARE: SHELF_1 OK
```

5. Kiểm tra dữ liệu `SHELF_1` trở về `0.0`.

Lưu ý: command service hiện vẫn gửi chuỗi `TARE: SHELF_1 OK` ngay cả khi HX711 không trả được mẫu. Nếu log có dòng `tare skipped: no valid samples`, thao tác tare thực tế đã thất bại và cần kiểm tra kết nối HX711.

### Bước 3: Cân bằng sai số bốn góc

Dùng cùng một quả cân chuẩn cho cả bốn góc. Chưa cần scale tuyệt đối chính xác ở bước này vì mục tiêu là so sánh tương đối giữa các góc.

1. Đặt quả cân tại góc 1, chờ số đo ổn định rồi gửi `CORNER1`.
2. Nhấc quả cân lên, đặt tại góc 2, chờ ổn định rồi gửi `CORNER2`.
3. Lặp lại với `CORNER3` và `CORNER4`.
4. Gửi:

```text
CORNERCHECK
```

Ví dụ kết quả:

```text
CORNERCHECK: avg=498.8g C1=-0.4%(496.8g) C2=0.7%(502.3g) C3=-0.1%(498.3g) C4=-0.2%(497.8g)
```

Ý nghĩa:

- `avg`: trung bình của bốn vị trí.
- `C1..C4`: phần trăm lệch của từng góc so với trung bình.
- Giá trị dương: góc đó đọc cao hơn trung bình.
- Giá trị âm: góc đó đọc thấp hơn trung bình.

Nếu độ lệch vượt quá ngưỡng yêu cầu, ví dụ `±0.5%`:

1. Điều chỉnh trim potentiometer tương ứng trên junction box.
2. Nếu góc đọc cao, giảm độ nhạy của góc đó.
3. Nếu góc đọc thấp, tăng độ nhạy của góc đó.
4. Thực hiện lại `TARE` khi mặt cân trống.
5. Đo lại đủ cả bốn góc; không chỉ đo lại riêng góc vừa chỉnh.
6. Lặp đến khi bốn góc nằm trong ngưỡng chấp nhận.

Nếu junction box không có trim potentiometer, firmware sử dụng một HX711 cho cả bốn load cell không thể tự bù độc lập sai số từng góc.

### Bước 4: Hiệu chỉnh scale bằng quả cân chuẩn

Sau khi cân bằng bốn góc:

1. Bỏ quả cân ra khỏi mặt cân.
2. Gửi lại `TARE` và xác nhận cân về 0.
3. Đặt quả cân chuẩn ở chính giữa mặt cân.
4. Chờ giá trị ổn định.
5. Nếu quả cân chuẩn là 500 g, gửi:

```text
CALIBRATE:500
```

6. Ghi lại phản hồi, ví dụ:

```text
CALIBRATE: OK scale=31.8425 raw_avg=-170556
```

7. Sau khi filter được nạp lại, kiểm tra `SHELF_1` hiển thị gần 500 g.

Không dùng dấu phẩy thập phân trong lệnh. Với quả cân 1.5 kg, sử dụng `CALIBRATE:1500`, không sử dụng `CALIBRATE:1,5`.

### Bước 5: Kiểm tra sau hiệu chỉnh

Thử lần lượt:

- Mặt cân trống: kết quả phải về 0.
- Đặt quả cân chuẩn ở giữa.
- Đặt cùng quả cân ở bốn góc.
- Đặt vật có khối lượng khác trong phạm vi tải cho phép.
- Nhấc vật ra và kiểm tra cân trở về 0.

Nên kiểm tra nhiều lần để đánh giá độ lặp lại, không chỉ độ chính xác của một lần đo.

## 9. Lưu hệ số hiệu chỉnh

`TARE` và `CALIBRATE` hiện chỉ cập nhật dữ liệu trong RAM:

- `offset` được tính lại tự động mỗi lần khởi động cho shelf 1 và shelf 2.
- `scale` từ lệnh `CALIBRATE` sẽ mất khi ESP32 reset hoặc mất nguồn.

Để giữ `scale` sau khi reset, lấy giá trị `scale` từ phản hồi UART và cập nhật cấu hình của đúng shelf trong:

```text
components/shelf_service/shelf_service.c
```

Ví dụ:

```c
.offset = -186477,
.scale = 31.8425f,
```

Sau đó build và flash lại firmware. Không cần chép offset đo được vào source nếu vẫn muốn sử dụng cơ chế auto-tare mỗi lần khởi động.

Firmware hiện chưa lưu hệ số hiệu chỉnh vào NVS. Shelf 1 và shelf 2 có lệnh tare/calibrate riêng; firmware không còn cấu hình shelf 3.

## 10. Xử lý sự cố

### `SHELF_n HX711 timeout`

Kiểm tra:

- Nguồn và GND của đúng HX711 được báo lỗi.
- Shelf 1: SCK GPIO25, DOUT GPIO34.
- Shelf 2: SCK GPIO26, DOUT GPIO35.
- Không có thiết bị khác điều khiển đường SCK tương ứng.
- Nếu shelf 1 timeout, shelf 2 vẫn tiếp tục được đọc và ngược lại.

### Trọng lượng luôn bằng 0

- Kiểm tra raw value trong log.
- Kiểm tra load cell và junction box.
- Đảm bảo `scale` khác 0 và có dấu phù hợp với chiều biến thiên raw.
- Các giá trị sau lọc từ `-3 g` đến `3 g` bị ép về 0 theo logic hiện tại.

### Trọng lượng dao động mạnh

- Chờ hệ thống ổn định nhiệt.
- Kiểm tra độ cứng của mặt cân và vị trí gá load cell.
- Tách dây tín hiệu khỏi relay, motor hoặc nguồn switching nhiễu cao.
- Đảm bảo nguồn HX711 ổn định và GND tốt.

### ESP32 không nhận lệnh UART

- Kiểm tra baudrate `115200`, định dạng `8N1`.
- Kiểm tra nối chéo TX/RX và GND chung.
- Gửi một lệnh trong mỗi packet/dòng.
- Dùng chữ hoa đúng như bảng lệnh.
- Gửi ký tự xuống dòng `\n` sau lệnh.

### `CALIBRATE: ERROR invalid weight`

Khối lượng truyền vào bằng 0, âm hoặc không parse được. Ví dụ hợp lệ:

```text
CALIBRATE:500
```

### `CALIBRATE: ERROR no samples`

Không nhận được mẫu hợp lệ từ HX711 của shelf đang hiệu chỉnh. Kiểm tra nguồn, GND, SCK và DOUT của shelf đó.

### `CALIBRATE: ERROR scale too small`

Giá trị raw khi đặt quả cân gần bằng offset. Kiểm tra:

- Quả cân đã được đặt lên mặt cân.
- Load cell có thay đổi raw khi chịu tải.
- Dây load cell và junction box được đấu đúng.
- Đã tare khi mặt cân trống, không tare khi đang đặt tải.

### `CORNERCHECK: ERROR missing CORNERn`

Chưa thu thập đủ bốn kết quả. Gửi lại lần lượt `CORNER1`, `CORNER2`, `CORNER3`, `CORNER4`, sau đó mới gửi `CORNERCHECK`.

## 11. Checklist chạy nhanh

1. Nối hai HX711 đúng chân, UART và GND chung.
2. Để mặt cân trống.
3. Build và flash firmware.
4. Mở monitor ở 115200 baud.
5. Gửi `PING`, kiểm tra nhận `READY`.
6. Gửi `STATUS`, kiểm tra trạng thái MC-38 đúng với cửa thực tế.
7. Gửi `UNLOCK`, mở rồi đóng cửa và kiểm tra đủ `SESSION: started`/`SESSION: ended`.
8. Gửi `TARE`, kiểm tra shelf 1 về 0.
9. Đo `CORNER1..4`, sau đó chạy `CORNERCHECK`.
10. Điều chỉnh trim pot và lặp lại nếu cần.
11. Đặt quả cân ở giữa và gửi `CALIBRATE:<gram>`.
12. Ghi lại scale mới và cập nhật source nếu muốn lưu cố định.
13. Build, flash lại và kiểm tra ở nhiều vị trí tải.

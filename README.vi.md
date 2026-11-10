# XPadMac

[English](README.md) · **Tiếng Việt**

XPadMac là một driver DriverKit cho macOS. Nó làm cho **tay cầm Xbox One / Xbox Series có dây (giao thức GIP)** và **tay cầm Xbox 360 có dây của các hãng thứ ba (XInput)** mà macOS bỏ qua hiện ra như một **Xbox Wireless Controller** (`045E:0B13`). Từ đó các game dùng GameController framework, System Settings › Game Controllers, Safari (Gamepad API) và các ứng dụng IOHID đều thấy một tay cầm Xbox chuẩn, có cả rung.

XPadMac không phải kext và không cần tắt SIP. Phiên bản hiện tại là 0.1.0, bản phát hành sớm.

Đọc nhanh: [Yêu cầu](#yêu-cầu) · [Build và cài đặt](#build-và-cài-đặt) · [Sử dụng XPadMac](#sử-dụng-xpadmac) · [Xử lý sự cố](#xử-lý-sự-cố) · [Cập nhật](#cập-nhật-driver) · [Gỡ cài đặt](#gỡ-cài-đặt) · [Giới hạn và phân phối](#giới-hạn-và-phân-phối)

## Vì sao có XPadMac

**macOS chỉ điều khiển bốn tay cầm Xbox có dây.** Driver có sẵn của Apple, `/System/Library/DriverExtensions/XboxGamepad.dext`, chỉ khớp bốn mã sản phẩm của Microsoft: `045E:028E` (Xbox 360 có dây), `045E:02EA` (Xbox One S), `045E:0B00` (Elite Series 2) và `045E:0B12` (Xbox Series X|S). Không driver nào nhận các tay cầm GIP còn lại: tay cầm Xbox One đời đầu, Elite Series 1, và tay cầm, dock, receiver GIP của PDP, PowerA, Hori, Razer, 8BitDo và các hãng khác. macOS không bao giờ đặt configuration USB cho chúng, nên chúng không có interface nào. Với game, tay cầm như chưa được cắm.

**Tay cầm Xbox 360 của các hãng thứ ba cũng không có driver.** macOS cấu hình chúng (thường là thiết bị composite), nhưng không driver nào nhận interface XInput (`FF/5D/01`), nên game không thấy.

**Chế độ HID dự phòng không giúp được.** Một số tay cầm bên thứ ba tự chuyển sang chế độ tay cầm HID thường khi không có máy chủ GIP nào trả lời. Driver HID chung của macOS nhận chế độ đó, nhưng **GameController không liệt kê nó**: tay cầm đó không nằm trong số tay cầm Apple biết. XPadMac giữ tay cầm ở chế độ GIP bằng cách trả lời nó ngay.

**Vì sao không làm tay cầm ảo trong userspace.** Cách đơn giản nhất là đọc tay cầm thật bằng IOUSBHost hoặc libusb rồi tạo một thiết bị HID ảo (`IOHIDUserDevice`, hoặc `HIDVirtualDevice` của CoreHID). Nhưng API đó cần entitlement hạn chế `com.apple.developer.hid.virtual.device`:

- phải được Apple duyệt: Account Holder của team gửi một Capability Request, và có yêu cầu đã chờ nhiều tháng;
- không có bản "Development";
- kernel giết chương trình không có entitlement này, chạy bằng root cũng vậy. Cách lách duy nhất là tắt AMFI, tức là tắt SIP.

**Mọi entitlement DriverKit đều có bản "Development"**: team Apple Developer trả phí nào cũng dùng được mà không cần hỏi Apple, và một dext ký development vẫn chạy khi SIP bật. Vì thế XPadMac là một DriverKit extension.

**Vì sao phải giả làm tay cầm Xbox Bluetooth.** Với thiết bị HID do một dext không phải của Apple tạo ra, kernel chỉ cho khớp các personality GameController có `GCIOMatchVirtual=true`. Đó là các mã Xbox Bluetooth (`045E:0B13`, `0B20`…). Plugin `XboxOneHIDServicePlugin` của Apple còn đòi `VendorID` 0x045E, `Transport` khác `"USB"` và mã sản phẩm nằm trong danh sách của nó. Vì vậy driver mang danh tính của một Xbox Wireless Controller qua Bluetooth LE. Bằng chứng (disassembly, thread Apple DTS trên diễn đàn, mã Linux và SDL) nằm trong `docs/research/`; thiết kế chi tiết nằm trong [`docs/DESIGN.md`](docs/DESIGN.md) (tiếng Anh).

## XPadMac làm gì

```text
Tay cầm GIP (FF/47/D0)      ─► XPadGIPDevice ─► XPadGIPGamepad ──┐
Tay cầm XInput (FF/5D/01)   ─► XPad360Gamepad ───────────────────┼─► HID "Xbox Wireless Controller" 045E:0B13
                                                                 │   Transport BluetoothLowEnergy
                                                                 ▼
                                                  GameController · Safari · ứng dụng IOHID
```

XPadMac gồm ba phần:

- **XPadDriver**, DriverKit extension `<tiền tố>xpadmac.driver.dext`, nằm trong ứng dụng ở `Contents/Library/SystemExtensions`. Nó có bốn lớp:
  - **`XPadGIPDevice`** làm việc ở cấp thiết bị USB, giống `XboxUSBDevice` của Apple. Nó đọc configuration descriptor; nếu có interface GIP (`FF/47/D0`) thì đặt configuration để các interface hiện ra, nếu không thì từ chối thiết bị (ví dụ tay cầm Xbox 360 / XInput).
  - **`XPadGIPGamepad`** là máy chủ GIP trên interface 0 (`FF/47/D0`). Nó khởi động tay cầm, gửi ack, ghép các frame bị chia nhỏ và giữ kết nối (keep-alive); máy trạng thái lấy từ AnyPad, nằm trong `Shared/gip.c`. Nó đọc input `0x20` với độ chính xác đầy đủ (cần analog 16 bit, cò 10 bit), nút Guide và nút Share (bố cục Series), và gửi lệnh rung GIP cho cả bốn motor: hai motor chính và hai motor ở cò. Với các tay cầm cần, nó thực hiện **xác thực GIP thật**, phía máy chủ của hai kiểu bắt tay RSA (v1) và ECDH (v2), như xone trên Linux (`Shared/gip_auth.c`).
  - **`XPadGIPAudio`** đặt interface âm thanh GIP (interface 1) về alternate setting 0, như xone làm; một số tay cầm bên thứ ba đợi yêu cầu này. XPadMac không truyền âm thanh.
  - **`XPad360Gamepad`** nhận interface XInput (`FF/5D/01`) của tay cầm Xbox 360 có dây của các hãng thứ ba (`Shared/xpad_x360.c`), có rung hai motor.
- **XPadMac.app**, ứng dụng SwiftUI có bốn mục: **Trình điều khiển**, **Tay cầm**, **Tần số báo cáo** và **Chẩn đoán USB** (xem [Sử dụng XPadMac](#sử-dụng-xpadmac)).
- **xpadctl**, công cụ chẩn đoán dòng lệnh (`doctor`, `hid`, `gc`, và `gip` khi build kèm libusb).

Driver công bố danh tính sau:

- `VendorID` 0x045E, `ProductID` 0x0B13, `VersionNumber` 0x0517;
- `Transport` `"BluetoothLowEnergy"`, `Product` `"Xbox Wireless Controller"`, `Manufacturer` `"Microsoft"`;
- report descriptor dùng nguyên 283 byte của một tay cầm thật qua BLE;
- thuộc tính chẩn đoán `XPadSource`, có giá trị `"GIP vvvv:pppp"` hoặc `"X360 vvvv:pppp"`, cho biết tay cầm thật.

### Tay cầm được hỗ trợ

Danh sách này lấy từ 43 `IOKitPersonalities` trong `Driver/Info.plist`.

| Hãng | VID | Điều kiện khớp | Personality |
| --- | --- | --- | --- |
| Microsoft Xbox One (2013, 2015), Elite Series 1 | `045E` | chỉ `02D1`, `02DD`, `02E3` | `XPadGIPDevice_Microsoft`, `XPadGIPGamepad_Microsoft`, `XPadGIPAudio_Microsoft` |
| 8BitDo | `2DC8` | mọi sản phẩm GIP | `…_8BitDo` |
| GameSir | `3537` | mọi sản phẩm GIP | `…_GameSir` |
| Hori | `0F0D` | mọi sản phẩm GIP | `…_Hori` |
| Hyperkin | `2E24` | mọi sản phẩm GIP | `…_Hyperkin` |
| Mad Catz | `0738` | mọi sản phẩm GIP | `…_MadCatz` |
| PDP | `0E6F` | mọi sản phẩm GIP | `…_PDP` |
| PowerA | `24C6`, `20D6` | mọi sản phẩm GIP | `…_PowerA`, `…_PowerA_BDA` |
| Razer | `1532` | mọi sản phẩm GIP | `…_Razer` |
| Turtle Beach | `10F5` | mọi sản phẩm GIP | `…_TurtleBeach` |
| Mọi hãng trên trừ Microsoft, tay cầm Xbox 360 (XInput) | như hàng của hãng | interface 0 `FF/5D/01` (255/93/1) trên thiết bị macOS đã cấu hình (composite) | `XPad360Gamepad_<Hãng>` |

"Mọi sản phẩm GIP" nghĩa là: `XPadGIPDevice_<Hãng>` khớp các thiết bị có `bDeviceClass` 255 (vendor-specific) của hãng đó, `XPadGIPGamepad_<Hãng>` khớp interface 0 và `XPadGIPAudio_<Hãng>` khớp interface 1 có class/subclass/protocol `FF/47/D0` (255/71/208). Thiết bị class 255 nào không có interface GIP sẽ bị `XPadGIPDevice` từ chối.

Tay cầm cần xử lý riêng (vendor quirk) được nhận ra theo VID, như xone và SDL làm. Một quirk có thể khiến driver chờ tay cầm gửi Hello dù lâu bao nhiêu, xác thực GIP thật theo mặc định, hỏi số serial, hoặc gửi các yêu cầu descriptor kiểu Windows trước khi cấu hình thiết bị. Các quirk được mô tả trong `docs/DESIGN.md`.

Phần lớn các hãng trong bảng được khớp theo cách Linux xpad và xone khớp, tức là theo VID và interface GIP/XInput, và **chưa được thử trên phần cứng thật**. Nếu tay cầm của bạn chạy (hoặc không chạy), hãy [mở issue](https://github.com/hiepkay/Xpadmac/issues) kèm kết quả `xpadctl doctor`.

### Không hỗ trợ

- **Bluetooth.** macOS đã tự hỗ trợ tay cầm Xbox Series và Xbox One S qua Bluetooth; XPadMac chỉ dùng USB (cáp, hoặc dock hay receiver USB dùng giao thức GIP).
- **Receiver không dây của Xbox 360** (`FF/5D/81`), và tay cầm Xbox 360 có thiết bị USB thuộc class vendor (`bDeviceClass` 255, như tay cầm của chính Microsoft): chỉ thiết bị composite, loại mà macOS tự cấu hình, mới được nhận interface XInput.
- **Âm thanh** (cổng tai nghe, chat), mức pin (descriptor 283 byte không có báo cáo pin), điều khiển đèn LED, và phần mềm cấu hình hay cập nhật firmware của hãng.
- **Tay cầm Microsoft mà macOS đã điều khiển** (`045E:028E`, `02EA`, `0B00`, `0B12`): driver của Apple vẫn giữ chúng, và `XPad360Gamepad` không bao giờ khớp VID của Microsoft.
- **Chế độ HID dự phòng của tay cầm bên thứ ba** (khi tay cầm đã rời GIP): rút ra cắm lại để nó khởi động ở GIP.

## Yêu cầu

- Mac dùng **Apple silicon** hoặc chip **Intel**. XPadMac được phát triển trên macOS 27.2, máy Apple silicon. Ứng dụng khai báo tối thiểu macOS 14, nhưng các bản trước 27 chưa được thử. Ứng dụng và driver được build cho cả arm64 lẫn x86_64, nhưng máy Intel chưa được thử.
- SIP **bật**, như bình thường. XPadMac không cần tắt SIP.
- **Xcode** có DriverKit SDK (phát triển với Xcode 27), và Command Line Tools (`clang`, `python3`).
- **[XcodeGen](https://github.com/yonaskolb/XcodeGen) 2.46 trở lên**: `brew install xcodegen` ([Homebrew](https://brew.sh)).
- Một team **Apple Developer Program trả phí**, đăng nhập trong Xcode. Team cá nhân miễn phí không có entitlement DriverKit. Để `make build` tự đăng ký máy Mac và tạo App ID cùng profile, vai trò của bạn trong team phải là Admin hoặc Account Holder; nếu không, nhờ một Admin thêm Provisioning UDID của máy (xem [Bước 2](#bước-2-đăng-ký-máy-mac)).
- Mật khẩu quản trị của máy, để cho phép driver.
- Tuỳ chọn: `brew install libusb` để có lệnh `xpadctl gip`; `openssl` cho bước kiểm tra vector xác thực trong `make test` (bỏ qua nếu không có).

## Build và cài đặt

Chạy mọi lệnh dưới đây trong thư mục gốc của mã nguồn (thư mục có `Makefile`). `make help` liệt kê các target và in team, tiền tố đang dùng. Có thể chạy `make test` trước để kiểm tra phần lõi C và các personality; lệnh này không cần tài khoản.

### Bước 0: Tải mã nguồn

```sh
git clone https://github.com/hiepkay/Xpadmac.git
cd Xpadmac
```

Các lệnh ở những bước dưới đây chạy trong thư mục này.

### Bước 1: Đặt team và tiền tố bundle ID

1. Mở **Xcode › Settings… › Accounts**, bấm **+**, chọn **Apple ID** và đăng nhập bằng tài khoản thuộc một team trả phí.
2. Lấy Team ID: [developer.apple.com/account](https://developer.apple.com/account) › Membership details.
3. Tạo file `Config.local.xcconfig` ở thư mục gốc. File này không được đưa vào git; đừng sửa `Config.xcconfig`, file đó để trống team và dùng tiền tố giữ chỗ `com.example.`:

   ```text
   DEVELOPMENT_TEAM = ABCDE12345
   BUNDLE_ID_PREFIX = com.yourname.
   ```

   Tiền tố kết thúc bằng dấu chấm và phải là của bạn (ví dụ một tên miền đảo ngược bạn sở hữu), vì mỗi App ID chỉ thuộc về một team. Hai bundle ID sẽ là:
   - ứng dụng: `<tiền tố>xpadmac`;
   - driver: `<tiền tố>xpadmac.driver`.

   `Makefile` đọc hai giá trị này từ `Config.xcconfig`, rồi từ `Config.local.xcconfig`; dòng lệnh hoặc biến môi trường được ưu tiên hơn (`make build TEAM=… PREFIX=…`). `make build` dừng ngay, kèm hướng dẫn, nếu chưa có team, nếu tiền tố vẫn là `com.example.`, hoặc nếu tiền tố không kết thúc bằng dấu chấm.

### Bước 2: Đăng ký máy Mac

Driver ký development chỉ chạy trên máy Mac mà provisioning profile của nó liệt kê theo **Provisioning UDID** của máy. Xem UDID trong **System Information › Hardware › Provisioning UDID**, hoặc bằng lệnh:

```sh
system_profiler SPHardwareDataType | grep 'Provisioning UDID'
```

`make build` truyền `-allowProvisioningDeviceRegistration` cho Xcode, nên Xcode tự đăng ký máy vào team của bạn (cần vai trò Admin hoặc Account Holder). Nếu không, thêm máy bằng tay ở [Certificates, Identifiers & Profiles › Devices](https://developer.apple.com/account/resources/devices/list) với loại thiết bị **macOS**.

> **Cạm bẫy trên Apple silicon: Hardware UUID và Provisioning UDID.** Trên Mac Apple silicon, System Information hiện cả *Hardware UUID* lẫn *Provisioning UDID*, và hai mã này khác nhau. Profile phải liệt kê **Provisioning UDID**. Nếu trước đây máy được đăng ký theo Hardware UUID (các công cụ cũ làm vậy), profile Mac App Development mà Xcode tự tạo cho ứng dụng sẽ gắn với bản ghi đó, và macOS từ chối chạy ứng dụng ("No matching profile found"). Đăng ký thêm Provisioning UDID sau đó cũng không sửa được ứng dụng: khi đó portal ghi nó là thiết bị iOS ("iPod"), loại mà profile macOS App Development bỏ qua. Profile DriverKit của driver có liệt kê bản ghi đó, nên driver không sao; chỉ ứng dụng cần một trong hai cách sau:
>
> - `make build APP_PROFILE="<tên>"`: ký ứng dụng thủ công bằng một profile **macOS App Development** lấy từ portal có liệt kê Provisioning UDID (xcodebuild tải profile theo tên);
> - `make build APP_DEVID_PROFILE="<tên>"`: ký lại ứng dụng bằng một profile **Developer ID**, loại profile không liệt kê thiết bị nào. Tạo một profile Developer ID cho App ID của ứng dụng (`<tiền tố>xpadmac`, có capability System Extension), tải về và bấm đúp để cài; trong keychain phải có chứng chỉ Developer ID Application cùng khoá riêng của nó. Xcode không chịu nhúng một dext ký bằng chứng chỉ khác, nên ứng dụng và driver vẫn được build như thường, rồi `Tools/resign-app.py` chỉ ký lại lớp vỏ ngoài của ứng dụng (có hardened runtime, bỏ `get-task-allow`). Driver giữ nguyên chữ ký và profile development của nó, và macOS chỉ đòi hai phần cùng một team.

### Bước 3: Build có ký

```sh
make build
```

Lệnh này:

1. kiểm tra team và tiền tố (`check-signing`);
2. tạo `XPadMac.xcodeproj` từ `project.yml` (XcodeGen);
3. chạy `xcodebuild … -allowProvisioningUpdates -allowProvisioningDeviceRegistration`: đăng ký máy nếu cần, tạo App ID và profile development có các entitlement `DriverKit`, `DriverKit USB Transport` và `DriverKit Family HID Device` (bản Development) cho driver, và `System Extension` cho ứng dụng;
4. đặt cho bản build một số phiên bản riêng, `CFBundleVersion` = giờ UTC dạng NGÀY.GIỜ.PHÚT với NGÀY đếm từ 2020-01-01, để macOS coi mỗi bản build là một driver mới;
5. kiểm tra driver nằm trong ứng dụng với các personality hợp lệ (`make check-product`), và mỗi profile đi kèm đều liệt kê Provisioning UDID của máy này và cấp mọi entitlement hạn chế mà gói tương ứng được ký kèm (`make check-profiles`). Một trong hai bước kiểm tra lỗi thì build lỗi.

Lần đầu, macOS có thể hỏi có cho `codesign` dùng khoá trong keychain không; chọn **Always Allow**. Build thành công thì dòng cuối là:

```text
Built (signed for team …, build …): build/DerivedData/Build/Products/Debug/XPadMac.app
```

`make build-unsigned` build cùng mã nhưng không ký, nên không cần tài khoản hay team (dùng được với tiền tố `com.example.`). Bản đó chỉ để kiểm tra biên dịch; **driver của nó không cài được**.

### Bước 4: Cài ứng dụng vào /Applications

```sh
make install
```

Lệnh này xoá bản cũ `/Applications/XPadMac.app` nếu có, chép bản vừa build vào đó bằng `ditto`, rồi mở ứng dụng. macOS chỉ kích hoạt system extension từ ứng dụng nằm trong `/Applications`; nếu chạy từ chỗ khác, mục **Trình điều khiển** sẽ cảnh báo "Ứng dụng chưa nằm trong /Applications". `make install` cũng cảnh báo nếu ứng dụng hoặc driver không có provisioning profile đi kèm (bản `make build-unsigned`).

### Bước 5: Bật driver và cho phép nó

1. Trong XPadMac, mở mục **Trình điều khiển** và bật công tắc driver (hoặc bấm **Cài đặt driver**). Trạng thái chuyển sang **Đang chờ bạn cho phép**.
2. Bấm **Mở Cài đặt hệ thống**, vào **System Settings › General › Login Items & Extensions › Driver Extensions** (trên macOS tiếng Việt: **Cài đặt hệ thống › Cài đặt chung › Mục đăng nhập & tiện ích mở rộng › Tiện ích mở rộng driver**). Bật **XPadMac** và nhập mật khẩu quản trị.
3. Quay lại ứng dụng. Trạng thái phải là **Đã cài đặt và đang chạy**, kèm thông báo hãy rút ra và cắm lại tay cầm. Nếu ứng dụng báo cần khởi động lại để hoàn tất, hãy khởi động lại máy.

### Bước 6: Cắm lại tay cầm

1. Thoát các chương trình có thể đang giữ thiết bị USB của tay cầm, nhất là **Google Chrome** (Gamepad API / WebUSB). Mỗi thiết bị USB chỉ một chương trình mở được tại một lúc; nếu Chrome đang giữ tay cầm, driver không mở được nó.
2. Rút cáp USB ra (cáp của tay cầm, hoặc của dock hay receiver) rồi cắm lại. **Không giữ nút nào** trong lúc cắm.
3. Tay cầm không dây qua dock hoặc receiver: bấm nút Xbox để bật tay cầm. Một số receiver chỉ báo có tay cầm khi tay cầm đã kết nối.

Lúc này tay cầm phải hiện trong mục **Tay cầm** của ứng dụng và trong System Settings › Game Controllers với tên **Xbox Wireless Controller**.

## Sử dụng XPadMac

### Ứng dụng

- **Trình điều khiển**: trạng thái driver và công tắc bật/tắt (có cả trên thanh công cụ của cửa sổ). Bật là kích hoạt driver; tắt (sau khi xác nhận) là vô hiệu hoá nó. Driver đã cài nhưng bị tắt trong Cài đặt hệ thống thì chỉ bật lại được ở đó, nên trong trường hợp này công tắc sẽ mở Cài đặt hệ thống. **Cài đặt driver** (**Cài lại / cập nhật driver** khi đã cài) cài driver đi kèm ứng dụng, hoặc thay bản đang cài bằng nó. **Thông tin kỹ thuật** cho biết mã driver, phiên bản driver trong ứng dụng và vị trí ứng dụng. Nếu driver trong ứng dụng có cùng phiên bản với bản đã cài, ứng dụng sẽ báo: khi đó macOS giữ nguyên bản đang chạy.
- **Tay cầm**: những tay cầm GameController đang thấy, mỗi tay cầm có phần xem trực tiếp, trong đó mọi nút, hai cần analog, hai cò và D-pad sáng lên khi bạn dùng. **Thử rung** rung lần lượt motor trái, motor phải, cò trái, cò phải rồi tất cả, mỗi lượt một giây (chỉ những motor mà GameController báo có).
- **Tần số báo cáo**: chọn một tay cầm HID, bấm **Bắt đầu đo**, và đo số báo cáo đầu vào mỗi giây tới macOS. Mục này hiện tần số hiện tại (1 giây), trung bình, cao nhất và tần số khai báo (`ReportInterval`), thống kê khoảng cách giữa các báo cáo (nhỏ nhất, trung bình, trung vị, p99, lớn nhất, jitter) và biểu đồ 30 giây gần nhất. Thiết bị có nhãn **XPadMac** là tay cầm do driver tạo ra, nên con số đo được là tần số game nhận; thiết bị không có nhãn cho tần số thô của chính nó. Hãy xoay liên tục cả hai cần analog khi đo: tay cầm GIP chỉ gửi báo cáo khi có thay đổi, và khoảng lặng trên 250 ms được tính là khoảng nghỉ, không tính là khoảng cách. Thiết bị được mở chung, nên game vẫn chạy bình thường trong lúc đo.
- **Chẩn đoán USB**: các tay cầm Xbox đang cắm qua USB (thiết bị class 255 của các hãng được hỗ trợ, và mọi thiết bị có interface GIP hoặc XInput), cách mỗi thiết bị được cấu hình, và lớp driver nào đang gắn vào nó (`XPadGIPDevice` / `XPadGIPGamepad` / `XPadGIPAudio` / `XPad360Gamepad` của XPadMac, của Apple, hoặc không có), kèm gợi ý. Mục này tự làm mới khi cắm hoặc rút thiết bị.

### xpadctl

```sh
make tool                          # build Tools/xpadctl/xpadctl (thêm WITH_LIBUSB=1 để có lệnh gip)
Tools/xpadctl/xpadctl doctor       # kiểm tra toàn bộ, kèm gợi ý; chỉ đọc, không gửi gì tới tay cầm
Tools/xpadctl/xpadctl gc           # tay cầm theo cách GameController thấy, 10 lần/giây (Ctrl-C để thoát)
```

| Lệnh | Tác dụng |
| --- | --- |
| `xpadctl doctor` | Kết luận ✅/⚠️/❌ kèm gợi ý cho: hệ thống, SIP, thiết bị USB, tay cầm HID, GameController, system extension XPadMac, và ứng dụng (có trong /Applications, có driver đi kèm, được ký để macOS chịu cài). Thoát với mã 1 nếu có mục ❌. |
| `xpadctl hid [--vid V --pid P] [--all] [--seconds N]` | In từng báo cáo HID thô, kèm trạng thái đã giải mã cho thiết bị XPad. Mặc định: `045E:0B13`; `--vid`/`--pid` chọn thiết bị khác (chỉ in byte thô); `--all` in mọi báo cáo, không chỉ khi có thay đổi. |
| `xpadctl gc [--seconds N]` | Xem trực tiếp tay cầm qua GameController trong terminal: tên, nút, cần analog và cò. |
| `xpadctl gip [tuỳ chọn]` | Chỉ có khi `make tool WITH_LIBUSB=1`. Chạy máy chủ GIP trong userspace bằng libusb, để thử giao thức mà không cần driver. `--auth`, `--fake-auth` và `--serial` ép dùng xác thực thật, "xác thực xong" giả, hoặc yêu cầu số serial; `xpadctl gip --help` liệt kê các tuỳ chọn còn lại. Lệnh này chiếm thiết bị USB và gửi lưu lượng GIP tới tay cầm, nên **đừng chạy nó khi driver đang điều khiển tay cầm**. |

Khi mọi thứ chạy đúng, các dòng chính của `xpadctl doctor` trông như sau (rút gọn, một tay cầm GIP `vvvv:pppp`; `xpadctl` chỉ in tiếng Anh):

```text
== USB devices (Xbox controllers: GIP and XInput) ==
✅ vvvv:pppp "…" — GIP, driver XPadGIPDevice, XPadGIPAudio, XPadGIPGamepad running

== HID controllers (IOHIDManager) ==
✅ 045e:0b13 "Xbox Wireless Controller" transport BluetoothLowEnergy — XPad device
     XPadSource = "GIP vvvv:pppp", driver XPadGIPGamepad

== GameController ==
✅ "Xbox Wireless Controller" — category "…", extendedGamepad

== XPadMac driver (System Extension) ==
✅ The driver is enabled (activated enabled).

== App ==
✅ /Applications/XPadMac.app 0.1.0 (2505.9.41) (driver included, signed)
```

Mục **App** đọc chữ ký của cả ứng dụng lẫn driver. Nếu nó báo ứng dụng chưa được ký đúng để macOS chịu cài driver, thì `/Applications/XPadMac.app` là bản `make build-unsigned` (hoặc thiếu entitlement hay profile): build lại bằng `make build` rồi `make install`.

### Ngôn ngữ

Ứng dụng theo ngôn ngữ của macOS: tiếng Anh, hoặc tiếng Việt khi tiếng Việt là ngôn ngữ ưu tiên. `xpadctl` chỉ in tiếng Anh.

## Xử lý sự cố

Luôn bắt đầu bằng `Tools/xpadctl/xpadctl doctor`: nó cho biết macOS đang thấy tay cầm ở mã nào, driver nào đang nhận nó và chương trình nào đang giữ nó.

> Trong zsh, shell mặc định của macOS, `log` là lệnh có sẵn của shell. Vì vậy các lệnh dưới đây gọi `/usr/bin/log`; gõ `log stream …` sẽ báo lỗi `too many arguments`.

### Log của driver

Mọi dòng log của driver đều bắt đầu bằng `XPad:`. Chạy lệnh dưới đây rồi rút ra cắm lại tay cầm. Log cho thấy tay cầm được khớp ở mã nào, quá trình khởi động và xác thực GIP, và lỗi pipe USB nếu có:

```sh
/usr/bin/log stream --predicate 'eventMessage CONTAINS "XPad:"'
```

Với tay cầm cần xác thực, một lần bắt tay thành công sẽ log `auth v1: client finish verified` (hoặc v2), rồi `tx auth complete`.

### Driver kẹt ở "terminating for upgrade"

`systemextensionsctl list` hiện driver cũ là `[terminating for upgrade via delegate]`, hoặc ứng dụng báo thay đổi có hiệu lực sau khi khởi động lại: macOS không gỡ được driver khi còn thiết bị đang dùng nó. **Hãy khởi động lại máy.** Lần sau, rút tay cầm (và dock hay receiver) ra trước khi cập nhật hoặc tắt driver; khi đó không cần khởi động lại.

### Chương trình khác đang giữ tay cầm

Mục USB của `doctor` có dòng "programs using this USB device: …" (chương trình đang dùng thiết bị USB này), kèm tên và pid của từng chương trình. Thủ phạm hay gặp là Google Chrome; ngoài ra là bất kỳ chương trình nào mở thẳng thiết bị USB (libusb, WebUSB), kể cả `xpadctl gip`. Thoát chương trình đó rồi rút ra cắm lại tay cầm. Mục Tần số báo cáo báo `kIOReturnExclusiveAccess` khi một ứng dụng khác đang giữ riêng một thiết bị HID.

### Tay cầm tự chuyển sang chế độ HID

Một số tay cầm bên thứ ba rời GIP và chuyển sang chế độ HID dự phòng (với mã sản phẩm khác) khi không có máy chủ GIP trả lời kịp, ví dụ khi tay cầm được cắm trước lúc driver chạy. GameController không thấy chế độ đó. Khi driver đã chạy, rút ra cắm lại tay cầm (với tay cầm không dây, tắt rồi bật lại tay cầm), **không giữ nút nào**. Nếu tay cầm có tổ hợp nút để chọn chế độ (xem hướng dẫn của hãng), chọn chế độ Xbox / PC (XInput) thay vì Android / HID.

### Nút Xbox (Home) không có tác dụng trong một ứng dụng

macOS cũng xử lý tay cầm Xbox thật như vậy: nó giữ nút Home cho một thao tác hệ thống và chỉ trao nút đó cho ứng dụng **ở phía trước** có yêu cầu nhận nút. Mục Tay cầm của XPadMac có yêu cầu, nên nút sáng lên ở đó khi XPadMac ở phía trước; `xpadctl gc` hiện nút này là `Home`.

### Driver không nạp

1. **Trạng thái.** Xem mục **Trình điều khiển** trong ứng dụng, hoặc chạy:

   ```sh
   systemextensionsctl list | grep -i xpadmac
   ```

   - `[activated enabled]`: driver đang chạy.
   - `[activated waiting for user]`: chưa cho phép, làm lại [Bước 5](#bước-5-bật-driver-và-cho-phép-nó).
   - Không có dòng nào: chưa cài, bật driver trong ứng dụng.
2. **Vị trí ứng dụng.** Ứng dụng phải chạy từ `/Applications/XPadMac.app`. Nếu macOS chạy nó từ một bản sao tạm (App Translocation), ứng dụng sẽ cảnh báo. Chạy lại `make install`.
3. **Chữ ký, profile và entitlement.** Các thông báo sau đều do bản build không được ký đúng:
   - "Ứng dụng thiếu quyền cài system extension": bản build không ký (`make build-unsigned`);
   - "Chữ ký mã của driver không hợp lệ…" hoặc "macOS từ chối driver khi kiểm tra…": profile không có máy này, hoặc ứng dụng và driver không cùng một team.

   Kiểm tra bằng các lệnh sau:

   ```sh
   DEXT=$(ls -d /Applications/XPadMac.app/Contents/Library/SystemExtensions/*.dext)
   system_profiler SPHardwareDataType | grep 'Provisioning UDID'       # UDID của máy này
   security cms -D -i "$DEXT/embedded.provisionprofile" | plutil -p - | grep -A3 -E '"ProvisionedDevices"|"TeamIdentifier"'
   codesign -d --entitlements - "$DEXT"                                # driverkit, transport.usb, family.hid.device
   codesign -dv "$DEXT" 2>&1 | grep -E 'Authority|TeamIdentifier'     # Apple Development, cùng team với ứng dụng
   python3 Tests/check_profiles.py /Applications/XPadMac.app "$DEXT"   # những gì make build kiểm tra
   ```

   Nếu profile thiếu UDID, thêm máy vào team (xem [Bước 2](#bước-2-đăng-ký-máy-mac), cả phần cạm bẫy trên Apple silicon), rồi `make build && make install` và bật lại driver.
4. **Log kích hoạt.** Mở lệnh này trong một cửa sổ Terminal rồi bật driver:

   ```sh
   /usr/bin/log stream --style compact --predicate 'process == "sysextd" OR process == "kernelmanagerd" OR eventMessage CONTAINS[c] "dext"'
   ```

### Driver đang chạy nhưng tay cầm không hiện ra

- Đọc mục **USB devices** của `doctor`:
  - "GIP controller with no driver: macOS ignores it (not configured, no interfaces)": không personality nào khớp. Hãng của tay cầm không có trong [bảng](#tay-cầm-được-hỗ-trợ), hoặc driver chưa được bật.
  - "XPadGIPDevice configured it, but XPadGIPGamepad has not attached to interface 0": xem log của driver.
  - "configured, but no XPad driver": một chương trình hoặc driver khác đang giữ interface; xem [mục trên](#chương-trình-khác-đang-giữ-tay-cầm).
- Nếu `doctor` báo "The XPad driver created a HID device, but GameController lists no controllers": rút ra cắm lại. Nếu vẫn vậy, [mở issue](https://github.com/hiepkay/Xpadmac/issues) kèm kết quả `xpadctl doctor` và log của driver.

## Cập nhật driver

Sau khi sửa mã hoặc kéo bản mới về:

1. Rút tay cầm (và dock hay receiver) ra, để macOS thay được driver đang chạy mà không cần khởi động lại.
2. Chạy `make build && make install`. Mỗi lần `make build` đặt một số build mới, nên macOS thấy đây là phiên bản driver mới và thay bản cũ. macOS chỉ thay driver khi phiên bản khác: nếu cùng phiên bản, nó giữ nguyên driver đang chạy mà vẫn báo thành công (ứng dụng sẽ cảnh báo khi điều đó xảy ra, ví dụ khi build thẳng trong Xcode; hãy tăng `CURRENT_PROJECT_VERSION` hoặc build bằng `make build`).
3. Trong ứng dụng, bấm **Cài lại / cập nhật driver**, rồi cắm tay cầm vào.
4. Nếu ứng dụng báo cần khởi động lại, hoặc `systemextensionsctl list` hiện `terminating for upgrade`, **hãy khởi động lại máy**.

## Gỡ cài đặt

1. Rút tay cầm ra. Trong XPadMac, mở mục **Trình điều khiển**, gạt tắt công tắc driver và xác nhận **Tắt driver**. Nếu ứng dụng báo driver sẽ tắt hẳn sau khi khởi động lại, hãy khởi động lại máy.
2. Chạy `make uninstall` để xoá `/Applications/XPadMac.app`.
3. Tuỳ chọn:
   - `make clean` xoá các sản phẩm build;
   - xoá `Config.local.xcconfig`;
   - App ID, profile và máy đã đăng ký vẫn nằm trong tài khoản developer; xoá chúng ở [Certificates, Identifiers & Profiles](https://developer.apple.com/account/resources/) nếu muốn.

**Thứ tự quan trọng.** Xoá ứng dụng trước sẽ để driver lại trong hệ thống, và khi SIP bật thì `systemextensionsctl uninstall` không chạy được. Nếu lỡ xoá ứng dụng trước, hãy `make install` lại rồi tắt driver.

## Giới hạn và phân phối

- **Bản ký development chỉ chạy trên Mac đã đăng ký.** Driver chỉ được kích hoạt trên máy có Provisioning UDID nằm trong profile development của team. Vì vậy không thể đưa file `.app` đã build cho người khác dùng: mỗi người tự build với team của mình, hoặc team đăng ký máy của họ (tối đa 100 máy Mac mỗi năm thành viên) rồi build lại. Chứng chỉ và profile development có hạn; khi hết hạn thì chạy lại `make build`.
- **Phân phối rộng cần Apple duyệt.** Để ký Developer ID và notarize, Account Holder của team phải xin Apple các entitlement DriverKit bản phân phối:
  - `com.apple.developer.driverkit`;
  - `com.apple.developer.driverkit.transport.usb`, ghi từng VID dạng thập phân (ví dụ 3695 cho PDP, `0E6F`);
  - `com.apple.developer.driverkit.family.hid.device`.

  Gửi yêu cầu tại [developer.apple.com/contact/request/system-extension](https://developer.apple.com/contact/request/system-extension/), hoặc ở tab Capability Requests của App ID ([hướng dẫn](https://developer.apple.com/help/account/capabilities/capability-requests/)). Việc duyệt có thể mất vài tuần hoặc lâu hơn. XPadMac chưa có các entitlement này. (`APP_DEVID_PROFILE` chỉ ký lại ứng dụng; driver vẫn ký development, nên nó không gỡ được giới hạn này.)
- **Danh tính Microsoft là mượn.** Driver công bố `045E:0B13` "Xbox Wireless Controller", Manufacturer "Microsoft", Transport `BluetoothLowEnergy`, dù tay cầm thật cắm USB và không do Microsoft sản xuất. Hệ quả:
  - game hiện tên và biểu tượng nút Xbox;
  - công cụ cần danh tính thật (cập nhật firmware, phần mềm cấu hình của hãng) không nhận ra tay cầm;
  - ứng dụng có thể coi tay cầm là không dây;
  - không có mức pin.

  Apple chưa công bố chính sách nào về việc một dext công bố VID/PID của công ty khác; hãy hỏi Apple DTS khi xin entitlement phân phối.
- **Mới thử với ít tay cầm**: phần lớn các hãng trong bảng chưa được thử trên phần cứng thật.

## Cấu trúc dự án

| Đường dẫn | Nội dung |
| --- | --- |
| `Shared/` | lõi C99 dùng chung cho driver, `xpadctl` và kiểm thử: `xpad.h` và `gip.h` (hợp đồng giữa các phần), bộ dựng báo cáo Xbox, descriptor 283 byte, máy chủ GIP `gip.c`, xác thực `gip_auth.c` và phần mật mã `gip_crypto.c`, bộ dịch Xbox 360 `xpad_x360.c` |
| `Driver/` | DriverKit extension (`XPadGIPDevice`, `XPadGIPGamepad`, `XPadGIPAudio`, `XPad360Gamepad`), `Info.plist` (personality), entitlement |
| `App/` | ứng dụng SwiftUI XPadMac |
| `Tools/xpadctl/` | công cụ chẩn đoán |
| `Tools/resign-app.py` | ký lại ứng dụng bằng một profile Developer ID (`make build APP_DEVID_PROFILE=…`) |
| `Tests/` | kiểm thử phần lõi, kiểm tra personality (`check_personalities.py`), entitlement (`check_entitlements.py`) và profile (`check_profiles.py`) |
| `docs/` | [`DESIGN.md`](docs/DESIGN.md) và bằng chứng nghiên cứu (`research/`) |
| `project.yml`, `Config.xcconfig`, `Makefile` | spec XcodeGen, thiết lập build dùng chung, các lệnh build |

Các lệnh `make`:

```sh
make test             # kiểm thử lõi C, kiểm tra Shared/ build cho DriverKit, descriptor, vector xác thực, personality, entitlement
make tool             # build Tools/xpadctl/xpadctl  (make tool WITH_LIBUSB=1: thêm lệnh gip)
make project          # tạo XPadMac.xcodeproj từ project.yml
make build-unsigned   # build ứng dụng và driver không ký (máy nào cũng được, không cần tài khoản)
make build            # build có ký (Apple Development)
make check-product    # kiểm tra driver trong ứng dụng và các personality của nó
make install          # chép vào /Applications/XPadMac.app và mở
make uninstall        # xoá /Applications/XPadMac.app
make clean            # xoá sản phẩm build
```

## Kiểm thử

`make test` (tức là `make -C Tests`) chỉ cần công cụ của Xcode, không cần tài khoản. Lệnh này:

- kiểm tra `Shared/*.c` build được cho DriverKit và không dùng gì ngoài libSystem và hai hook của nền tảng;
- kiểm tra `Shared/xpad_xbox_desc.c` đúng là thứ `Tests/gen_xbox_desc.py` tạo ra từ descriptor đã ghi lại, và `Tests/auth_vectors.h` đúng là thứ `Tests/gen_auth_vectors.py` tạo ra từ các khoá thử có trong repo (đối chiếu bằng `openssl`; bỏ qua nếu không có python3 hoặc openssl);
- kiểm tra mọi personality trong `Driver/Info.plist` dùng tổ hợp khoá mà IOUSBHostFamily khớp được, và entitlement của driver đúng bốn khoá nó cần;
- chạy các kiểm thử đơn vị, build kèm sanitizer address và undefined-behaviour: descriptor, báo cáo Xbox và phép co giãn giá trị, các message Xbox 360, máy chủ GIP chạy lại theo một bản ghi khởi động thật (`docs/research/gip_bringup_capture.log`) cộng fuzzing, các hàm mật mã so với vector công bố, và hai kiểu bắt tay xác thực v1, v2 với một tay cầm giả lập.

Các target chi tiết hơn được liệt kê ở đầu `Tests/Makefile`: ví dụ `make -C Tests test` chỉ chạy kiểm thử đơn vị, `SAN=` build không kèm sanitizer, và `XPAD_TEST_VERBOSE=1` in những gì mã GIP ghi log.

## Ghi công

- **[AnyPad PS5](https://codeberg.org/elmonomalva0/Any-Pad-ps5)** của **elmonomalvad0**, và bản port **[AnyPad PS4](https://github.com/Vld439/AnyPad-PS4)** của nó: `Shared/gip.c` và `Shared/gip.h` phái sinh từ một module GIP viết thêm trên AnyPad PS4 và chưa từng được công bố trong dự án đó (máy trạng thái khởi động tay cầm, các vendor quirk, ack, frame chia nhỏ, lọc module). Module được giữ nguyên trong `docs/research/anypad_gip.c.orig` và `anypad_gip.h.orig`, kèm `pad.h` của AnyPad mà nó dùng trong `anypad_pad.h.orig`; xem [`docs/research/README.md`](docs/research/README.md).
- **[xone](https://github.com/medusalix/xone)** của **Severin von Wnuck-Lipinski** và những người đóng góp (bản fork đang được duy trì: [dlundqvist/xone](https://github.com/dlundqvist/xone)), GPL-2.0-or-later: `Shared/gip_auth.c` (phía máy chủ của xác thực GIP) và PRF trong `Shared/gip_crypto.c` phái sinh từ `auth/auth.c` và `auth/crypto.c`; ngoài ra là quy tắc chia frame và ack, yêu cầu số serial, và lệnh đặt lại interface âm thanh.
- **[Linux xpad](https://github.com/torvalds/linux/blob/master/drivers/input/joystick/xpad.c)**: bố cục input của GIP và Xbox 360, vị trí nút Share, message khởi tạo và LED của 360, danh sách hãng.
- **[SDL](https://github.com/libsdl-org/SDL)**: các driver HIDAPI Xbox One và Xbox 360 (bước nhận dạng, bố cục input theo firmware), danh sách VID/PID.
- **[xpadneo](https://github.com/atar-axis/xpadneo)**: report descriptor BLE 283 byte "unified" của Xbox Wireless Controller và định dạng báo cáo rung (các bit bật motor).
- Các câu trả lời của Apple DTS trên [Developer Forums](https://developer.apple.com/forums/thread/809202) về bản Development của các entitlement DriverKit.

Xem [`NOTICE`](NOTICE) để có đầy đủ thông tin ghi công.

## Giấy phép

XPadMac phát hành theo **Giấy phép Công cộng GNU (GNU General Public License), phiên bản 3 hoặc (tuỳ bạn chọn) bất kỳ phiên bản nào mới hơn** (GPL-3.0-or-later); xem [`LICENSE`](LICENSE) và [`NOTICE`](NOTICE). Đây là điều bắt buộc: `Shared/gip.c` phái sinh từ mã GPL-3.0-or-later của AnyPad và `Shared/gip_auth.c` từ mã GPL-2.0-or-later của xone. Nếu bạn phát hành phiên bản riêng, kể cả bản binary, hãy kèm mã nguồn, giữ các thông báo bản quyền, phát hành các thay đổi theo cùng giấy phép, và ghi công các dự án gốc.

## Nhãn hiệu

Xbox, Xbox One, Xbox Series X|S và Xbox Wireless Controller là nhãn hiệu của Microsoft Corporation. Tên sản phẩm và công ty khác là nhãn hiệu của chủ sở hữu tương ứng. XPadMac là một dự án độc lập, **không liên kết với, không được Microsoft, Apple hay bất kỳ hãng tay cầm nào bảo trợ hoặc chứng nhận**.

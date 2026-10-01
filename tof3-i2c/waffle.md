# UNO Q + ROS2 Jazzy + TurtleBot3 Waffle 연동 학습 노트

> **2026-09-29 업데이트**: 구성이 바뀌었습니다. **라즈베리파이는 이제 안
> 씁니다.** ToF 센서 3개는 UNO Q의 자체 MCU(`sketch.ino`, 이 `tof3-i2c`
> 프로젝트)가 아니라 **RP2350 보드**가 I2C로 직접 읽고, UNO Q는 USB 허브를
> 통해 RP2350·OpenCR과 연결되는 "허브 컴퓨터" 역할을 맡습니다. rviz2는
> Windows의 WSL에서 돌립니다. 아래는 이 구성 기준으로 새로 쓴 문서입니다.
>
> **지금까지 만든 `sketch.ino`/`tof3-i2c-notes.md`는 코드로는 더 이상
> 안 쓰이지만, 거기서 확인한 교훈(풀업 저항값, 공유 버스에서 전류·GND가
> 겹치는 문제)은 RP2350 배선에도 그대로 적용됩니다** — 뒤에서 다시
> 언급합니다.

이 문서의 목적: **UNO Q(Debian Trixie)에서 Docker로 컨테이너 다루는 법**과
**여러 개의 ROS2 Jazzy 참여자(RP2350·UNO Q·OpenCR·WSL)를 네트워크로 연동
하는 법**을 익히는 것입니다.

---

## 0. 새 하드웨어 구성

```
[전원]
  보조배터리 A (USB-PD 출력)
      │ PD로 충전하며 전력 공급
      ▼
  USB 허브 (PD-in 지원, 전력 패스스루)
      ├─ UNO Q            ← 허브에서 전원 공급받음
      ├─ RP2350           ← 허브에 USB로 연결 (데이터 + 전원)
      └─ OpenCR           ← 허브에 USB로 연결 (데이터만)
                              OpenCR 자체는 보조배터리 B로 별도 구동

[센서 → 컴퓨팅]
  ToF 센서 3개 --I2C--> RP2350 --USB--> USB 허브 --USB--> UNO Q

[제어]
  UNO Q --USB(허브 경유)--> OpenCR --> TurtleBot3 Waffle 모터/IMU

[시각화]
  UNO Q (ROS2 Docker 컨테이너) --Wi-Fi/DDS--> Windows PC의 WSL2 --> rviz2
```

핵심 변화 정리:

| | 이전 계획 | 지금 계획 |
|---|---|---|
| ToF 센서를 읽는 주체 | UNO Q 자체 MCU (`sketch.ino`) | **RP2350** (I2C) |
| ToF ↔ ROS2 연결 | UNO Q 내부에서 HTTP(`/api/status`) 폴링 | **RP2350 ↔ UNO Q, USB 시리얼** |
| 회피 로직/OpenCR 제어 | 라즈베리파이 | **UNO Q** (라즈베리파이 역할을 그대로 넘겨받음) |
| rviz2 | 별도 PC(막연히) | **Windows WSL2** (구체적으로 지정됨 — §5에 주의사항 있음) |
| UNO Q 전원 | 자체 USB 충전 | **USB 허브를 통해 공급** (§4에 확인할 점) |

---

## 1. 보드에서 직접 확인한 사실 (UNO Q 쪽, 추측 아님)

| 항목 | 확인 결과 |
|---|---|
| CPU 아키텍처 | `aarch64` (ARM64) |
| OS | Debian GNU/Linux 13 (trixie) |
| Docker | `26.1.5` 설치돼 있고 실제로 동작 중 (Arduino App 자체도 Docker 컨테이너로 돌아감) |
| 현재 사용자(`arduino`) 권한 | 이미 `docker` 그룹 소속 — `sudo` 없이 `docker` 명령 바로 사용 가능 |
| **디스크 여유 공간** | **9.8GB 중 8.2GB 사용, 여유는 단 1.1GB** ⚠️ 아래 §2 필독 |
| RAM | 총 3.6GB, 사용 가능 여유 약 1.7GB |
| 인터넷 연결 | 정상 (Docker Hub 접속 확인됨) |
| ROS2 Jazzy 이미지 ARM64 지원 | `ros:jazzy-ros-base` 이미지에 `arm64/v8` 아키텍처가 실제로 존재함을 확인 (`docker manifest inspect`) |
| 기존 ROS2 흔적 | 없음 — 완전히 새로 시작 |

**RP2350·OpenCR·USB 허브는 UNO Q에 아직 연결되지 않은 상태라 제가 직접
확인은 못 했습니다** — 이 문서의 §3~§4는 "확인된 사실"이 아니라 "이렇게
하면 될 것이다"라는 **권장안**입니다. 실제로 연결한 뒤 증상이 다르면
바로 알려주세요.

---

## 2. 제일 먼저 할 일: 디스크 공간 확보 (건너뛰면 실패합니다)

여유 공간 1.1GB로는 ROS2 이미지를 안전하게 받기 부족합니다. 안 쓰는
Docker 이미지가 1.568GB 있으니 먼저 정리하세요:

```sh
docker image prune -a   # 안전함 — 나중에 그 예제를 다시 실행하면 자동 재다운로드
df -h /                 # 확보됐는지 확인
```

`ros:jazzy-ros-base`(GUI 없는 최소 구성, 아래 이유 있음)는 700MB~1GB
선이라 정리 후 여유(약 2.6GB)로 받을 수 있지만 넉넉하진 않습니다.
`ros:jazzy-desktop`/`-full`은 수 GB라 **받지 마세요.**

---

## 3. RP2350에 ToF 센서 3개 배선하기 — 지금까지의 교훈이 그대로 적용됩니다

이 프로젝트에서 UNO Q로 겪었던 문제들(풀업 저항이 너무 세서 통신이 깨짐,
전원/GND가 부족해서 센서가 멈춤)은 **어느 MCU를 쓰든 똑같이 적용되는
전기적인 문제**입니다. RP2350에 옮겨 배선하실 때 아래를 그대로 챙기세요.

### 3.1 실측했던 값 (재사용 가능)

- 센서 3개 각각의 자체 풀업 저항: **1237Ω · 1008Ω · 1017Ω**
- 3개를 한 버스에 물리면 병렬로 **~360Ω까지 하락** (권장 1,000~4,700Ω보다
  훨씬 셈) → 통신 오류의 핵심 원인이었음
- 2개만 공유해도 **~509Ω** — 여전히 셈

→ RP2350에서도 **센서 3개를 전부 한 버스에 몰지 말고 나누세요.**
RP2350(RP2350 칩 기준)은 하드웨어 I2C 컨트롤러가 **2개**(`i2c0`, `i2c1`)
있어서, UNO Q에서 했던 것과 똑같이 "2개는 한 버스, 1개는 다른 버스"로
나눌 수 있습니다(또는 각 버스에 있는 센서들의 풀업 중 하나만 남기고
나머지를 제거하면 1개 버스로도 가능 — §풀업 확인 방법은
`tof3-i2c-notes.md`에 그대로 있습니다).

### 3.2 전원/GND 교훈

- 센서 3개의 VCC-GND 전압을 **그 센서 자신의 GND 핀 기준**으로 측정 —
  펌웨어 로딩 순간(전류를 가장 많이 먹는 구간) 전압이 떨어지는지 확인.
- GND는 굵고 짧게, 가능하면 공통 지점까지 각자 따로("스타 배선").
- **RP2350은 지금 USB 허브를 통해 전력을 공급받을 예정**이라(§4), 허브
  자체의 전류 여유도 같이 확인해야 합니다 — RP2350 + 센서 3개가 동시에
  피크 전류를 먹는 순간(펌웨어 부팅)을 허브가 감당하는지가 관건입니다.

### 3.3 RP2350 ↔ UNO Q 통신 방식 (두 가지 선택지)

**A안 — 간단한 USB 시리얼 브리지 (처음엔 이걸 권장)**

RP2350 펌웨어가 센서 3개 값을 JSON 텍스트로 만들어 USB CDC 시리얼로
그냥 흘려보내고, UNO Q의 ROS2 컨테이너 안에서 작은 Python 노드가 그
시리얼 포트를 읽어서 ROS2 토픽으로 발행합니다. 구조가 단순하고, 이번에
UNO Q로 만들었던 `sketch.ino`의 센서 읽기 로직(주소 지정, LPn, 8×8
읽기)을 RP2350용 SDK(Pico SDK C/C++ 또는 MicroPython)로 옮기기만 하면
됩니다 — 로직 자체는 거의 그대로 재사용 가능합니다.

**B안 — RP2350에서 micro-ROS를 직접 실행**

RP2350이 micro-ROS 에이전트를 통해 USB로 직접 ROS2 토픽을 발행하는
방식입니다. 이러면 UNO Q 쪽엔 micro-ROS 에이전트만 띄우면 되고 별도
브리지 코드가 필요 없어집니다. 다만 **RP2350(라즈베리파이 Pico 2 칩)에
대한 micro-ROS 지원이 실제로 어느 정도 성숙했는지는 제가 이 세션에서
확인할 방법이 없어서 장담은 못 드립니다** — RP2040(Pico 1세대)은 지원이
잘 돼 있는 걸로 알고 있지만, RP2350은 비교적 최신 칩이라 확인이
필요합니다. 학습 단계인 지금은 **A안으로 먼저 확실하게 동작시키고,
익숙해지면 B안을 실험**하시길 권합니다.

---

## 4. 전원 구성 확인할 점

말씀하신 구성(USB 허브에 PD로 충전하며 UNO Q·RP2350에 전력 공급, OpenCR은
별도 배터리)은 원리적으로 가능하지만, **허브가 실제로 "PD 패스스루로
다운스트림 기기에 전력을 공급"하는 기능을 지원하는 모델인지**가 관건입니다
— 저가형 허브 중엔 "허브 자체만 그 전력으로 동작하고 다운스트림엔 안 주는"
것도 있고, "PD 입력을 받아도 5V/낮은 전류만 다운스트림에 내보내는" 것도
있습니다. 이 프로젝트 내내 겪었던 "전류 부족 → 순간적으로 멈춤" 문제가
**UNO Q 자체에도 똑같이 재현될 수 있는 지점**이라, 실제 연결 후:

1. 허브 제품 스펙에서 "PD pass-through" 또는 "power delivery to host"
   같은 표현이 있는지 확인하세요.
2. UNO Q가 ROS2 컨테이너를 돌리며 부하가 걸린 상태에서, RP2350도 동시에
   부팅(펌웨어 로딩, 전류 최대 구간)하는 순간에 UNO Q가 재부팅되거나
   느려지지 않는지 관찰하세요.
3. 이상 증상이 보이면 이전과 같은 방법(그 기기 자신의 GND 기준 VCC-GND
   실측)으로 확인하시면 됩니다.

---

## 5. rviz2를 Windows WSL2에서 돌릴 때 반드시 확인할 것

**WSL2는 기본적으로 자체 가상 네트워크(NAT)를 씁니다.** ROS2의 DDS는
기기들이 서로를 자동으로 찾을 때 멀티캐스트를 쓰는데, WSL2의 기본
네트워크 모드에서는 이 멀티캐스트가 Windows 밖(같은 Wi-Fi의 UNO Q)으로
안 나갈 수 있습니다. 이걸 모르고 진행하면 "분명히 같은 `ROS_DOMAIN_ID`인데
rviz2에 아무것도 안 뜬다"는 상황이 됩니다.

해결책(권장 순서):

1. **Windows 11 + 최신 WSL이면 "미러링 네트워크 모드"를 켜세요.**
   Windows 사용자 폴더의 `.wslconfig` 파일(없으면 새로 생성)에:
   ```ini
   [wsl2]
   networkingMode=mirrored
   ```
   추가하고 `wsl --shutdown` 후 다시 시작하면, WSL2가 Windows 호스트와
   같은 네트워크를 그대로 써서 멀티캐스트가 정상 동작합니다. **이게
   된다면 가장 깔끔한 해결책입니다.**
2. 미러링 모드를 쓸 수 없는 버전이라면(구형 Windows/WSL), DDS를
   유니캐스트 방식으로 강제 설정해야 합니다(예: CycloneDDS의
   `Peers` 설정에 UNO Q의 IP를 직접 명시). 이건 상황이 되면 그때
   구체적으로 같이 잡아드리겠습니다.
3. Windows 방화벽이 WSL↔외부 통신을 막고 있지 않은지도 확인하세요.

---

## 6. UNO Q가 라즈베리파이 역할을 대신하는 부분 (OpenCR 제어)

라즈베리파이를 안 쓰기로 하셨으니, 원래 라즈베리파이가 맡던 **OpenCR과
USB 시리얼로 통신하며 모터 명령(`cmd_vel`)을 보내고 IMU/오도메트리를
받는 역할**을 이제 UNO Q의 ROS2 컨테이너가 맡아야 합니다. 이건 보통
ROBOTIS의 `turtlebot3_node` 패키지가 담당합니다.

확인이 필요한 부분(제가 이 세션에서 실물 없이 확정할 수 없는 것들):

- `turtlebot3_node`/`turtlebot3_bringup` 패키지의 **ROS2 Jazzy + ARM64**
  조합 배포 여부 — 바이너리 패키지가 없으면 소스 빌드가 필요할 수 있고,
  그러면 §2에서 확보한 디스크 공간이 또 부족해질 수 있습니다(빌드
  도구·의존성이 꽤 큽니다). 실제로 진행하실 때 `apt-cache search
  turtlebot3` 등으로 먼저 확인해보시길 권합니다.
- OpenCR이 USB 허브를 거쳐 컨테이너 안에 `/dev/ttyACM0` 같은 이름으로
  정상적으로 보이는지 — `docker run` 시 `--device=/dev/ttyACM0` 형태로
  명시적으로 넘겨줘야 컨테이너 안에서 보입니다. RP2350의 시리얼 포트도
  마찬가지로 별도 `--device`가 필요합니다(둘 다 있으면 `/dev/ttyACM0`,
  `/dev/ttyACM1`처럼 번호가 매겨지는데, **꽂는 순서에 따라 번호가 바뀔 수
  있어서** udev 규칙으로 고정 이름을 만들어두는 걸 권장합니다).

---

## 7. 배경 지식 (컨테이너·ROS2 기본 개념)

- **컨테이너/Docker**: 필요한 OS 환경을 통째로 담은 상자. UNO Q가
  Debian이어도 Ubuntu 기반 ROS2 Jazzy 컨테이너가 문제없이 돌아갑니다.
- **노드/토픽**: ROS2에서 프로그램(노드)들이 이름 있는 통로(토픽)로
  데이터를 주고받습니다. 이름이 정확히 같아야 연결됩니다.
- **ROS_DOMAIN_ID**: 연동하려는 모든 기기(UNO Q, WSL, RP2350이 micro-ROS를
  쓴다면 그것도)가 **같은 번호**를 써야 서로를 찾습니다.
- **rviz2를 UNO Q에서 안 돌리는 이유**: 화면도 없고 RAM도 빠듯해서,
  화면이 있는 별도 컴퓨터(지금은 Windows/WSL)에서 네트워크로 구독해서
  보는 구조입니다.

---

## 8. 설치·실행 단계 (UNO Q 쪽)

### 8.1 ROS2 Jazzy 이미지 받기

```sh
docker pull ros:jazzy-ros-base
```

### 8.2 컨테이너 실행 — `--network host` + USB 장치 전달

```sh
docker run -it --network host \
  --device=/dev/ttyACM0 \
  --device=/dev/ttyACM1 \
  --name ros_jazzy ros:jazzy-ros-base bash
```

`--network host`가 필요한 이유는 §5에서 설명한 것과 같습니다 — DDS
멀티캐스트가 격리된 컨테이너 네트워크를 못 벗어나기 때문입니다.
`--device`는 RP2350·OpenCR의 시리얼 포트를 컨테이너 안에서 쓰게
해줍니다(실제 장치 이름은 연결 후 `ls /dev/ttyACM*`로 확인하세요).

### 8.3 확인

```sh
source /opt/ros/jazzy/setup.bash
ros2 topic list
```

### 8.4 RP2350 → ROS2 브리지 노드 (A안 기준)

RP2350이 USB 시리얼로 JSON을 흘려보낸다고 가정한 예시입니다(RP2350
펌웨어 쪽 출력 형식에 맞춰 파싱 부분만 바꾸면 됩니다):

```python
import json
import serial
import rclpy
from rclpy.node import Node
from std_msgs.msg import Float32

class TofBridge(Node):
    def __init__(self):
        super().__init__('tof_bridge')
        self.pubs = [
            self.create_publisher(Float32, f'/tof/sensor{i+1}/min_distance_mm', 10)
            for i in range(3)
        ]
        self.ser = serial.Serial('/dev/ttyACM0', 115200, timeout=0.5)
        self.timer = self.create_timer(0.05, self.poll)

    def poll(self):
        line = self.ser.readline()
        if not line:
            return
        try:
            data = json.loads(line)
        except ValueError:
            return
        for sensor in data.get('sensors', []):
            values = [v for v in sensor['distance_mm'] if v is not None]
            if values:
                msg = Float32()
                msg.data = float(min(values))
                self.pubs[sensor['id']].publish(msg)

def main():
    rclpy.init()
    rclpy.spin(TofBridge())

if __name__ == '__main__':
    main()
```

---

## 9. 주의할 점 모음

- **디스크 용량 계속 확인**(`df -h /`) — 특히 `turtlebot3_node`를 소스
  빌드하게 되면 크게 부족해질 수 있습니다.
- **rviz2·gazebo는 절대 UNO Q에서 실행하지 마세요** — WSL2/Windows
  쪽에서만.
- RP2350·OpenCR을 USB 허브에 물릴 때 **`--device`를 빠짐없이 컨테이너에
  넘겼는지** 확인 — 안 넘기면 컨테이너 안에서 그 장치가 아예 안 보입니다.
- **WSL2 네트워킹 모드**(§5)를 확인 안 하면 rviz2에 아무것도 안 뜨는
  채로 오래 헤맬 수 있는 가장 흔한 함정입니다.
- **USB 허브의 PD 패스스루 전력이 UNO Q+RP2350을 동시에 감당하는지**
  (§4) — 이번 프로젝트에서 겪은 "전류 부족" 문제가 형태만 바뀌어 다시
  나타날 수 있는 지점입니다.
- RP2350에 센서 3개를 배선할 때 **풀업 저항 병렬 문제를 그대로
  재현하지 마세요** — §3.1 수치를 참고해서 버스를 나누거나 풀업을
  정리하세요.
- 이 ROS2 컨테이너는 `arduino-app-cli app list`에 안 뜹니다 — `docker
  ps`로 따로 확인하세요.

---

## 10. 진행 순서 제안

1. §2 디스크 정리
2. RP2350에 센서 3개 배선(§3 교훈 적용) + 펌웨어(A안: JSON 시리얼 출력)
3. RP2350을 USB 허브 경유로 UNO Q에 연결, `/dev/ttyACM*`로 보이는지 확인
4. §8 ROS2 컨테이너 띄우고 브리지 노드로 토픽 발행 확인(`ros2 topic echo`)
5. USB 허브 전원 구성(§4) 연결 후 UNO Q 안정성 관찰
6. Windows WSL2 네트워킹 모드(§5) 설정 후 rviz2에서 토픽 보이는지 확인
7. OpenCR 연결(§6) — `turtlebot3_node` 설치 가능 여부부터 확인
8. 전부 개별 검증되면 실제 장애물 회피 로직 연결, Waffle 구동 테스트

막히는 단계의 정확한 오류 메시지/증상을 알려주시면 그 단계부터 같이
짚어드리겠습니다.

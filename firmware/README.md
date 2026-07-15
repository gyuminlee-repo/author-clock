# 작가시계 펌웨어 (ESP32-S3)

콘센트에 꽂아두면 계속 켜지는 하드웨어 작가시계. 매분 그 시각에 해당하는 한국 문학
문장을 표시하고, 문장 속 시각 표현을 반전 블록(검정 배경 + 흰 글자)으로 강조한다.

## 대상 보드

**Waveshare ESP32-S3-RLCD-4.2** (ESP32-S3-WROOM-1-N16R8, 16MB 플래시 + 8MB 옥탈 PSRAM).

- 패널: ST7305, 4.2인치 모노 1bpp 반사형, 400x300 가로. 백라이트 없음(주변광 반사).
- SPI: CLK=GPIO11, MOSI=GPIO12, DC=GPIO5, CS=GPIO40, RST=GPIO41, TE=GPIO6(옵션). 클럭 1MHz.
- RTC: PCF85063 (I2C SDA=GPIO13, SCL=GPIO14).
- 버튼: KEY=GPIO18 (active low). 짧게 누르면 시계 화면과 캘린더 화면을 전환.
- 전원: USB-C 5V 상시 급전 = 항상 켜짐(자동 절전/꺼짐 없음). 반사형이라 소비 전력이 낮다.

## 화면

- 시계 화면: 상단~중앙에 대형 HH:MM 숫자(96px)가 주인공. 그 아래 인용문(28px, 자동 줄바꿈,
  넘치면 말줄임), 최하단에 출처(작품 · 작가). 문장 속 시각 표현은 반전 블록으로 강조. 우상단에
  디더링 처리한 고양이 실사 아이콘(72x72) 상시 표시. 해당 분에 문장이 없으면 시각만 크게 표시.
- 캘린더 화면: 상단 YYYY년 M월(44px), 요일 헤더(일~토), 6주 그리드. 오늘 날짜는 반전 블록.
  우상단에 같은 고양이 아이콘(48x48) 표시.

## 왜 되는가 (실측)

- 정확 시각 문장 데이터 `data/quotes_min.json` = 431KB (1440분, 1497개, 시간대 근사 문장 제외).
- 데이터에 실제 쓰인 한글 1,275자뿐 -> subset 폰트가 작다.
- LVGL 폰트(1bpp): 28px, 44px, 그리고 숫자만 담은 96px(수 KB).
- 인용문 데이터는 C 배열(tools/embed_quotes.py 생성)로 컴파일되며, LVGL 드로우 버퍼는 8MB PSRAM 사용.

## 빌드

### 1. 데이터·폰트 생성 (보드 없이도 생성됨)

```bash
cd firmware/tools
python3 build_data.py      # -> ../data/quotes_min.json, ../data/glyphs.txt
python3 make_icon.py       # assets/cat_bayer_*.png -> ../main/cat_icon.c(72x72), cat_icon_48.c(48x48)
bash   build_fonts.sh      # SIL OFL Pretendard subset -> font_ko_28.c, font_ko_44.c, font_digits_96.c
cp font_ko_28.c font_ko_44.c font_digits_96.c ../main/
```

`build_data.py`, `make_icon.py`, `build_fonts.sh`는 커밋된다. 원본 TTF와 대용량
`font_*.c`는 gitignore(스크립트로 재생성).

### 1-b. 우상단 아이콘 풀 생성

우상단 도트 그림은 매 분 셔플백으로 한 장씩 바뀐다(풀 전체를 한 바퀴 돈 뒤에만
재등장하므로 같은 그림이 짧은 간격으로 나오지 않는다).

```bash
cd firmware/tools
python3 make_pool.py             # assets/pool_local 스캔 -> ../main/icon_pool.c
```

- `assets/pool_local/`: 스프라이트 드롭 폴더. gitignore. 투명 배경 PNG 를 넣고
  `make_pool.py` 를 다시 돌리면 자동 흡수된다(하위 폴더는 스캔 안 함).
- `main/icon_pool.c`: 생성물. 로컬 아트를 담을 수 있어 gitignore(폰트와 동일 정책).

`make_pool.py` 는 커밋된다. `pool_local/` 과 `icon_pool.c` 는 저작권 아트를 공개 repo 에
올리지 않으려고 gitignore 한다. 서드파티 스프라이트는 각자 기기용으로만 로컬에 두고,
라이선스는 본인 책임으로 확인한다.

### 2. WiFi 크리덴셜

```bash
cp firmware/main/wifi_secrets.h.example firmware/main/wifi_secrets.h
# wifi_secrets.h를 열어 SSID/PASSWORD 입력 (gitignore됨)
```

크리덴셜이 없거나 접속 실패 시 NTP를 건너뛰고 PCF85063 RTC 시각으로 계속 동작한다.

### 3. 펌웨어 빌드·플래시 (PlatformIO + ESP-IDF)

LVGL은 컴포넌트 레지스트리 대신 로컬 컴포넌트로 사용한다 (레지스트리 접속이 막힌
네트워크에서도 빌드되도록). 최초 1회 클론:

```bash
cd firmware
git clone --depth 1 --branch v9.4.0 https://github.com/lvgl/lvgl.git components/lvgl
rm -f components/lvgl/idf_component.yml   # 레지스트리 재접속 방지
python3 tools/embed_quotes.py             # 인용문 DB -> main/quotes_data.c
```

```bash
cd firmware
pio run -e rlcd42                 # 빌드
pio run -e rlcd42 -t upload       # 플래시
pio device monitor -b 115200      # 로그
```

## 동작

- 부팅: NVS -> RTC 시각으로 시스템 시간 seed -> 인용문 로드 -> 디스플레이/LVGL/UI -> WiFi/NTP(비동기).
- 시간 소스: NTP(WiFi) 성공 시 시스템 시간 갱신 + RTC 기록. 실패 시 RTC 시각 유지.
- 갱신: 매초 HH:MM 숫자 갱신, 분 경계에서 그 분의 문장·캘린더 갱신.
- 렌더: LVGL이 RGB565로 그린 뒤 flush 콜백에서 임계값(`< 0x7fff`)으로 흑백 변환, ST7305로 전송.

## 라이선스

- 코드: MIT.
- 폰트: Pretendard (SIL OFL 1.1).
- 고양이 아이콘: Wikimedia Commons "A white persian cat" by Magnus Bråth,
  CC BY-SA 2.0 를 1bpp Bayer 디더링 처리. 원본은 `assets/persian.jpg`.
- 인용문: 웹앱과 동일(공개저작 원문 + 번역).

# 작가시계 데스크톱 앱

작가시계 GitHub Pages 사이트(<https://gyuminlee-repo.github.io/author-clock/>)를 감싸는 Tauri 2 데스크톱 래퍼입니다. 프론트엔드를 따로 번들하지 않고, 프로덕션 웹뷰가 원격 Pages 사이트를 그대로 로드합니다.

## 기능

- 원격 Pages 사이트를 네이티브 창에 표시 (기본 520x400, 크기 조절 가능)
- 트레이 메뉴의 "항상 위" 체크로 always-on-top on/off (기본 on)
- 트레이 아이콘 좌클릭 시 창 복구(show/focus), "종료"로 앱 종료

## 로컬 빌드

먼저 Rust를 설치합니다(<https://rustup.rs>). 아이콘은 `app-icon.png`에서 `tauri icon`으로 생성합니다. 아래 명령은 모두 `desktop/` 디렉토리에서 실행합니다.

### Windows (PowerShell)

```powershell
# Rust 설치 후, MSVC 빌드 툴체인 필요 (Visual Studio Build Tools)
npx --yes "@tauri-apps/cli@^2" icon app-icon.png
npx --yes "@tauri-apps/cli@^2" build
```

산출물: `src-tauri/target/release/bundle/nsis/*.exe`, `src-tauri/target/release/bundle/msi/*.msi`

### macOS

```bash
npx --yes "@tauri-apps/cli@^2" icon app-icon.png
npx --yes "@tauri-apps/cli@^2" build
```

산출물: `src-tauri/target/release/bundle/dmg/*.dmg`

macOS 빌드는 서명하지 않으므로("unsigned"), 처음 실행 시 Finder에서 앱을 우클릭 후 "열기"를 선택해 Gatekeeper 경고를 통과해야 합니다.

전역 CLI를 쓰려면 `npm i -g @tauri-apps/cli` 후 `tauri icon app-icon.png` / `tauri build`로 대체할 수 있습니다.

## CI 아티팩트

`.github/workflows/desktop-build.yml`이 Windows/macOS 빌드를 담당합니다. GitHub 저장소의 Actions 탭에서 "desktop-build" 워크플로우를 `workflow_dispatch`로 수동 실행하거나, `desktop-app` 브랜치에 `desktop/**` 변경을 push하면 자동 실행됩니다. 실행 완료 후 해당 run 페이지 하단 Artifacts에서 `author-clock-windows`(.exe/.msi), `author-clock-macos`(.dmg)를 내려받습니다.

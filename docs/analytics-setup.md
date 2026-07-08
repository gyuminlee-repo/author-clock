# 방문 트래킹(analytics) 설치 가이드

작가시계는 GitHub Pages로 배포되는 정적 사이트다. 서버가 없으므로 트래킹은 모두 `index.html` 안에 스니펫을 붙이는 방식으로 설치한다.

## 공통 붙이기 위치

`index.html`의 `<head>` 끝에 아래 주석 마커가 있다.

```html
<!-- analytics: docs/analytics-setup.md 참조. 측정ID 발급 후 이 아래에 스니펫 붙여넣기 -->
```

발급받은 스니펫을 이 마커 **바로 아래**, `</head>` **위**에 붙인다. 옵션은 하나만 선택하는 것을 권장한다.

## 옵션 비교

| 옵션 | 비용 | 쿠키 | 개인정보 | 설치 난이도 | 추천 대상 |
|---|---|---|---|---|---|
| A. GA4 | 무료 | 사용 | 동의 배너 필요 소지 | 보통 | 상세 지표가 필요한 경우 |
| B. Cloudflare Web Analytics | 무료 | 없음 | 친화적 | 쉬움 | 가볍게 방문 수만 보고 싶은 경우 |
| C. GoatCounter | 무료(오픈소스) | 없음 | 매우 친화적 | 쉬움 | 프라이버시 우선, 셀프호스팅 선호 |

---

## 옵션 A (권장): Google Analytics 4

세밀한 지표(유입 경로, 체류 시간, 이벤트)가 필요하면 GA4를 쓴다.

1. `https://analytics.google.com` 접속 후 구글 계정 로그인.
2. 좌하단 톱니(관리) 클릭 → **속성 만들기** 클릭.
3. 속성 이름에 `작가시계` 입력 → 시간대 `대한민국`, 통화 `대한민국 원` 선택 → **다음** → 업종/규모 선택 → **만들기**.
4. **데이터 스트림** 화면에서 **웹** 선택.
5. 웹사이트 URL에 `https://gyuminlee-repo.github.io/author-clock/` 입력, 스트림 이름 `작가시계 웹` 입력 → **스트림 만들기**.
6. 생성된 스트림 상세에서 **측정 ID**(`G-XXXXXXXXXX` 형식)를 복사.
7. 아래 스니펫의 `G-XXXXXXXXXX`를 복사한 측정 ID로 바꾼 뒤, `index.html`의 주석 마커 아래에 붙인다.

```html
<!-- Google Analytics 4 -->
<script async src="https://www.googletagmanager.com/gtag/js?id=G-XXXXXXXXXX"></script>
<script>
  window.dataLayer = window.dataLayer || [];
  function gtag(){dataLayer.push(arguments);}
  gtag('js', new Date());
  gtag('config', 'G-XXXXXXXXXX');
</script>
```

8. 커밋 후 GitHub Pages 배포가 끝나면(보통 1~2분), GA4 좌측 **보고서 → 실시간**에서 본인 방문이 잡히는지 확인.

장점: 무료로 가장 풍부한 지표 제공. 단점: 쿠키를 사용해 지역/법률에 따라 동의 배너가 필요할 소지가 있고, 광고 차단기에 막힐 수 있다.

---

## 옵션 B: Cloudflare Web Analytics (쿠키리스, 무료)

쿠키 없이 방문 수와 유입 경로만 가볍게 보고 싶다면 이 옵션을 쓴다. Cloudflare로 도메인을 옮기지 않아도 된다.

1. `https://dash.cloudflare.com` 접속 후 로그인(계정 없으면 무료 가입).
2. 좌측 메뉴 **Analytics & Logs → Web Analytics** 클릭.
3. **Add a site** 클릭 → 호스트명에 `gyuminlee-repo.github.io` 입력 → 추가.
4. 발급된 **JS 스니펫**(`data-cf-beacon`에 토큰이 담긴 `<script>`)을 복사.
5. 주석 마커 아래에 붙인다. 형태는 아래와 같다.

```html
<!-- Cloudflare Web Analytics -->
<script defer src="https://static.cloudflareinsights.com/beacon.min.js"
  data-cf-beacon='{"token": "여기에_발급된_토큰"}'></script>
```

6. 커밋·배포 후 Cloudflare 대시보드에서 방문이 집계되는지 확인.

장점: 쿠키 없음, 동의 배너 불필요, 설치가 한 줄. 단점: GA4보다 지표가 단순하다.

---

## 옵션 C: GoatCounter (오픈소스, 개인정보 친화)

오픈소스 셀프호스팅 또는 무료 호스팅 인스턴스를 쓰고 싶다면 GoatCounter가 좋다.

1. `https://www.goatcounter.com` 접속 → **Sign up** 클릭.
2. Code(하위 도메인)에 `authorclock` 등 원하는 코드 입력, 이메일·비밀번호 입력 → 가입.
3. 로그인 후 **Settings → Sites** 에서 사이트 코드를 확인(`authorclock.goatcounter.com` 형태).
4. 아래 스니펫의 `MYCODE`를 본인 코드로 바꿔 주석 마커 아래에 붙인다.

```html
<!-- GoatCounter -->
<script data-goatcounter="https://MYCODE.goatcounter.com/count"
  async src="//gc.zgo.at/count.js"></script>
```

5. 커밋·배포 후 GoatCounter 대시보드에서 방문이 잡히는지 확인.

장점: 오픈소스, 쿠키 없음, 대시보드가 공개 설정 가능. 단점: 무료 인스턴스는 대용량 트래픽에 제한이 있다.

---

## PWA(서비스워커) 캐시 주의점

작가시계는 서비스워커(`sw.js`)로 정적 자산을 캐시하는 PWA다. `index.html`을 캐시 목록에 포함하고 있으면, 스니펫을 추가해도 재방문자의 브라우저가 **옛 캐시본**을 계속 보여줘 트래킹이 반영되지 않을 수 있다.

반영 절차는 다음과 같다.

1. `sw.js` 안의 캐시 버전 문자열(예: `const CACHE = 'author-clock-vN'`)에서 `N`을 한 단계 올린다. 이 한 줄만 바꾸면 서비스워커가 새 버전으로 인식하고 옛 캐시를 지운다.
2. `index.html`, `sw.js`를 함께 커밋·배포한다.
3. 사용자가 다음 방문 시 새 서비스워커가 활성화되면서 스니펫이 적용된다.
4. 본인 기기에서 즉시 확인하려면 브라우저 개발자도구 **Application → Service Workers → Unregister** 후 강력 새로고침(Ctrl+Shift+R) 한다.

캐시 버전을 올리지 않은 채 트래킹이 안 잡히면, 십중팔구 옛 서비스워커 캐시가 원인이다.

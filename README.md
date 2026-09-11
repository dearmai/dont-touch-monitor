# dont-touch-monitor

macOS에서 **자동 화면 꺼짐을 막는 프로세스**를 조회하고 PID를 지정해 종료하는 CLI.
C와 macOS 기본 프레임워크만 사용합니다. 실행할 때 별도 런타임, 패키지, `pmset` 파싱이 필요하지 않습니다.

## 실행

결과물은 `dist/dont-touch-monitor` 파일 하나입니다. macOS 12 이상을 빌드 대상으로 하며,
`make universal`로 Apple Silicon(arm64)과 Intel(x86_64)을 함께 지원합니다.

```sh
# 화면 꺼짐 방지 + 사용자 활동 알림 조회 (인자 없이 실행해도 동일)
./dist/dont-touch-monitor

# 시스템 잠자기를 막는 앱도 포함
./dist/dont-touch-monitor list --all

# 스크립트에서 사용할 JSON
./dist/dont-touch-monitor list --all --json

# 목록에서 확인한 PID로 대체하세요. 먼저 실행할 동작 확인
./dist/dont-touch-monitor kill 12345 --force --dry-run

# 일반 종료 신호 (SIGTERM)
./dist/dont-touch-monitor kill 12345

# 강제 종료 (SIGKILL): 미저장 작업이 손실될 수 있습니다
./dist/dont-touch-monitor kill 12345 --force

# 여러 프로세스를 명시적으로 지정
./dist/dont-touch-monitor kill 12345 12346 --force
```

`kill`은 `--all` 목록에 나오는 시스템 잠자기 방지 앱도 처리합니다.
기본 목록은 PID, 앱 / 프로세스, 방해 유형, 종료 가능 여부, 요청 내용의 표로 출력합니다.
한글 표시 폭에 맞춰 정렬하고 터미널 너비(80~120열)에 맞춰 열 너비를 조절합니다.
긴 이름과 요청 내용은 `...`으로 생략하며, `--json`에서 전체 내용을 확인할 수 있습니다.
출력의 `전송 완료`는 신호 전송 성공을 뜻합니다. 특히 SIGTERM은 앱이 무시할 수 있으므로
필요하면 목록을 다시 조회한 뒤 `--force`를 사용하세요. 앱의 helper나 서비스 관리자가
프로세스를 다시 실행할 수 있습니다.

## 표시 의미와 진단 범위

| 표시 | macOS 요청 | 의미 |
| --- | --- | --- |
| 화면 | `PreventUserIdleDisplaySleep` | 유휴 상태의 디스플레이 꺼짐 방지 |
| 활동 | `UserIsActive` | 사용자 활동을 알림. 정상 키보드·마우스 입력도 포함되므로 원인 확정이 아님 |
| 시스템 | `PreventUserIdleSystemSleep`, `PreventSystemSleep` | 시스템 잠자기 관련 요청. `--all`에서 표시 |

[IOPMCopyAssertionsByProcess](https://github.com/opensource-apple/IOKitUser/blob/master/pwr_mgt.subproj/IOPMLib.h)로
프로세스별 활성 전원 요청을 조회합니다. 비활성(level 0) 요청은 제외합니다.
[화면 잠자기 방지 요청](https://developer.apple.com/documentation/iokit/kiopmassertiontypepreventuseridledisplaysleep)과
[사용자 활동 알림](https://developer.apple.com/documentation/iokit/1557127-iopmassertiondeclareuseractivity)은 구분해서 표시합니다.

이 도구는 **수동 화면 잠금(Control-Command-Q)의 차단 여부**를 판정하지 않습니다.
화면 보호기·암호 요구 설정, 전원 요청 없이 생성되는 가상 입력, USB·커널 수준의 잠자기 방해는
진단 범위 밖입니다. 조회 결과는 순간 스냅샷이고, 요청이 있어도 현재 전원 정책에서
실제로 적용되는지는 다를 수 있습니다. 목록이 비어 있어도 화면 잠금이 정상이라는 뜻은 아닙니다.
다른 앱을 대신한 요청도 전원 요청을 소유한 프로세스 기준으로 표시합니다.

## 종료 동작

- 사용자가 명시한 PID만 처리하며, 관련 활성 요청이 없는 프로세스는 종료하지 않습니다.
- 현재 사용자 소유 프로세스만 허용합니다. root, 다른 사용자, 시스템 경로의 프로세스,
  자기 자신과 부모 프로세스, 신원 확인이 불가능한 대상은 보호합니다.
- 모든 지정 PID를 먼저 검증합니다. 하나라도 부적합하면 신호를 보내지 않습니다.
- 신호 전송 직전에 요청과 프로세스 시작 시각·실행 경로·소유자를 다시 확인합니다.
  macOS의 PID 기반 signal API 특성상 확인과 신호 사이의 아주 짧은 경합을 완전히 없앨 수는 없습니다.
- 실제 전송 도중 상태가 바뀌거나 오류가 나면 일부 대상만 처리될 수 있으며 종료 코드 1을 반환합니다.
- `--dry-run`은 동일한 대상 검증을 수행하고 신호를 보내지 않습니다.
- `sudo` 실행은 권장하지 않습니다. root 실행으로 보호 제한을 우회할 수 없습니다.

종료 코드: `0` 성공(빈 목록 포함), `1` 조회·대상 검증·신호 전송 실패, `2` 잘못된 인자.

## 빌드와 테스트

빌드에는 Xcode Command Line Tools와 `make`가 필요합니다. 테스트에만 Python 3가 필요합니다.

```sh
make universal  # 단일 Universal 바이너리 + 로컬 ad-hoc 서명
make test       # 테스트가 직접 만든 caffeinate/sleep 프로세스만 종료
```

`make`는 현재 Mac 아키텍처로 빌드합니다. 이미 Universal 결과물이 있으면 그대로 사용합니다.
`make clean`은 `dist`를 삭제합니다. 다른 Mac에 배포할 때 Developer ID 서명·공증은 별도이며,
현재 결과물은 로컬 ad-hoc 서명입니다.

PATH에 설치하려면:

```sh
mkdir -p "$HOME/.local/bin"
install -m 755 dist/dont-touch-monitor "$HOME/.local/bin/dont-touch-monitor"
```

`~/.local/bin`을 PATH에 추가하면 어느 디렉터리에서든 `dont-touch-monitor`로 실행할 수 있습니다.

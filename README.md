# dont-touch-monitor

macOS에서 **자동 화면 꺼짐을 막는 프로세스**를 조회하고 PID를 지정해 종료하는 CLI.
C와 macOS 기본 프레임워크를 사용하며, 별도 런타임이나 패키지 설치가 필요하지 않습니다.
현재 요청은 IOKit API로 조회하고, 과거 기록은 macOS에 기본 포함된 `/usr/bin/pmset -g log`에서 읽습니다.

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

# 최근 30분 요청과 화면 켜짐/꺼짐 기록
./dist/dont-touch-monitor history

# 조회 기간 변경, 시스템 잠자기 요청도 포함
./dist/dont-touch-monitor history --minutes 60 --all

# 과거 기록의 전체 내용
./dist/dont-touch-monitor history --json

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
기본 목록은 PID, 앱 / 프로세스, **부모 프로세스 (PID)**, 방해 유형, 종료 가능 여부,
요청 내용의 표로 출력합니다. 별도 옵션 없이 부모 앱을 볼 수 있습니다.
예를 들어 `caffeinate`를 RustDesk가 실행했다면 부모 열에 `RustDesk (883)`처럼 표시합니다.
한글 표시 폭에 맞춰 정렬하고 터미널 너비(현재 목록 100~160열, 기록 80~120열)에 맞춰 열 너비를 조절합니다.
긴 이름과 요청 내용은 `...`으로 생략하며, `--json`에서 전체 내용을 확인할 수 있습니다.
출력의 `전송 완료`는 신호 전송 성공을 뜻합니다. 특히 SIGTERM은 앱이 무시할 수 있으므로
필요하면 목록을 다시 조회한 뒤 `--force`를 사용하세요. 앱의 helper나 서비스 관리자가
프로세스를 다시 실행할 수 있습니다.

현재 목록의 JSON에는 `parent_pid`, `parent_name`, `ancestors`가 포함됩니다.
`ancestors`는 직접 부모부터 최대 8단계의 상위 프로세스를 PID·이름·실행 경로로 제공합니다.
조회 중 프로세스 시작 시각과 부모 PID를 재확인하며, 확인할 수 없으면 부모는
표에서 `확인 불가`, JSON에서 `null`과 빈 배열로 표시합니다.
현재 부모는 최초 실행자나 실제 입력 발생 앱과 다를 수 있습니다. 예를 들어 부모가 종료되면
`launchd`로 바뀔 수 있으며, 시스템 서비스가 다른 앱의 요청을 대신 처리할 수도 있습니다.
`kill <PID>`는 지정한 프로세스만 대상으로 하며 부모를 자동으로 종료하지 않습니다.

## 최근 요청 기록

`history`는 명령 시작 시각 기준 최근 **30분**을 시간순 표로 보여줍니다.
`--minutes N`으로 1~1440분을 지정할 수 있습니다. 시간 범위의 양 끝을 포함하며,
표의 시각은 현재 Mac 시간대, JSON의 `time`은 원본 로그의 날짜·시간대입니다.

| 시각 | PID | 앱 / 프로세스 | 이벤트 | 요청 내용 |
| --- | --- | --- | --- | --- |
| 17:00:00 | - | - | 화면 꺼짐 | |
| 17:00:05 | 12345 | caffeinate | 활동 생성 | caffeinate command-line tool |
| 17:00:05 | - | - | 화면 켜짐 | |

위는 출력 형식을 설명하는 예시입니다.

- `Created`(생성), `TurnedOn`(활성화)을 요청으로 표시합니다. 동일 요청의 `Summary`,
  `Released`, `TimedOut`, `TurnedOff` 기록은 중복 요청으로 세지 않습니다.
- 화면 켜짐·꺼짐 기록을 함께 표시해 요청 시각과 비교할 수 있습니다.
  같은 시각에 기록됐다는 이유만으로 특정 앱이 원인이라고 확정하지 않습니다.
- 기본은 화면 꺼짐 방지·사용자 활동 요청이며, `--all`은 시스템 잠자기 방지 요청도 포함합니다.
- 이미 종료된 프로세스도 당시 로그의 이름과 PID로 표시합니다. **과거 PID는 재사용될 수 있으므로**
  종료하려면 `list`에서 현재 프로세스를 다시 확인하세요.
- 전원 로그에는 부모 PID가 없어 종료된 `caffeinate`를 실행한 앱을 소급해서 알아낼 수 없습니다.
  화면 상태 알림 자체도 원인 PID를 제공하지 않으므로 PID와 앱을 `-`로 표시합니다.
- macOS가 보관한 로그 범위와 형식에 의존합니다. 로그가 누락·삭제됐으면 복원하지 못하며,
  기록이 없다고 해당 요청이 없었다는 뜻은 아닙니다. 사전 백그라운드 실행은 필요하지 않습니다.
- 시스템 로그 전체를 읽은 뒤 기간을 필터링하므로 로그가 크면 조회에 시간이 걸릴 수 있습니다.

JSON은 `from`, `to`, `minutes`, `scope`, `events`를 포함합니다. 각 이벤트는
`time`, `timestamp`(Unix 초), `pid`, `process`, `action`, `type`, `name`을 가지며,
화면 상태 이벤트의 `pid`와 `process`는 `null`입니다.

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
진단 범위 밖입니다. `list` 조회 결과는 순간 스냅샷이고, 요청이 있어도 현재 전원 정책에서
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

테스트는 로그 시간 범위·시간대 변환·요청 필터·정렬·잘못된 로그 처리와 실제 프로세스의
조회·종료를 검증합니다. `dist/history-test`는 테스트용 실행 파일이며 배포에 필요하지 않습니다.

`make`는 현재 Mac 아키텍처로 빌드합니다. 이미 Universal 결과물이 있으면 그대로 사용합니다.
`make clean`은 `dist`를 삭제합니다. 다른 Mac에 배포할 때 Developer ID 서명·공증은 별도이며,
현재 결과물은 로컬 ad-hoc 서명입니다.

PATH에 설치하려면:

```sh
mkdir -p "$HOME/.local/bin"
install -m 755 dist/dont-touch-monitor "$HOME/.local/bin/dont-touch-monitor"
```

`~/.local/bin`을 PATH에 추가하면 어느 디렉터리에서든 `dont-touch-monitor`로 실행할 수 있습니다.

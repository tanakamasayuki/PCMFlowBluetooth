# Changelog / 変更履歴

## Unreleased
- (EN) Repository scaffolding, specification (SPEC.md / SPEC.ja.md) and test harness.
- (JA) リポジトリの scaffolding、仕様(SPEC.md / SPEC.ja.md)、テストハーネス。
- (EN) Added the real two-board EspBle A2DP Sink integration fixture, covering SBC-to-PCM decode across suspend/resume and reconnect.
- (JA) EspBle A2DP SinkからPCM復号までを、suspend/resumeと再接続を含めて検証する実機2台fixtureを追加。
- (EN) Fixed `SbcDecoder::reset()` to clear synthesis-filter history, making restarted streams deterministic and preventing stale PCM transients.
- (JA) `SbcDecoder::reset()`で合成フィルタ履歴を消去し、再開ストリームの再現性と旧PCM transientの混入を修正。

# Changelog / 変更履歴

## Unreleased
- (EN) Repository scaffolding, specification (SPEC.md / SPEC.ja.md) and test harness.
- (JA) リポジトリの scaffolding、仕様(SPEC.md / SPEC.ja.md)、テストハーネス。
- (EN) Added the real two-board EspBle A2DP Sink integration fixture, covering SBC-to-PCM decode across suspend/resume and reconnect.
- (JA) EspBle A2DP SinkからPCM復号までを、suspend/resumeと再接続を含めて検証する実機2台fixtureを追加。
- (EN) Fixed `SbcDecoder::reset()` to clear synthesis-filter history, making restarted streams deterministic and preventing stale PCM transients.
- (JA) `SbcDecoder::reset()`で合成フィルタ履歴を消去し、再開ストリームの再現性と旧PCM transientの混入を修正。
- (EN) Extended the `reset()` regression to compare a freshly allocated decoder and a channel-count reconfiguration against the reset baseline — a reset-versus-reset comparison alone does not detect the uninitialized scratch area.
- (JA) `reset()`回帰試験を拡張し、新規確保したdecoderとchannel数再設定後の状態をreset基準と比較するようにした。reset同士の比較だけでは未初期化scratch領域を検出できないため。
- (EN) Added the A2DP validation report (docs/A2DP_VALIDATION_REPORT.md / .ja.md) and documented `ConcurrentUpdate` as a defensive path with no automated refusal test.
- (JA) A2DP検証レポート(docs/A2DP_VALIDATION_REPORT.md / .ja.md)を追加し、`ConcurrentUpdate`が自動テストを持たない防御的経路であることを明記。

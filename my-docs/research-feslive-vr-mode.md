# 調査メモ: fes live のオーバーレイカメラ操作を VR モード化できるか

## 1. 前提（今回固定する条件）

- 実装対象は **現状のオーバーレイ方式を最大限再利用**する
- ただし入力経路の方針は以下へ更新する
  - ❌ VRコントローラー入力を `WASD/QERF` へ直接割り当てる
  - ✅ **VRコントローラー入力から、ゲーム側カメラAPIを直接呼ぶ**
- まず目指すものは「簡易VR」
  - 映像は **単一画面を両目表示**（stereo は後回し）
  - HMD頭部トラッキングは **いったん無視**
- 開発・実行環境
  - Windows PC
  - BlueStacks 5 でアプリ実行
  - Meta Quest 3 を安定接続して利用

---

## 2. 現状整理（今回の要点）

今回やりたいことは、説明として次の形に揃えること。

- 現状（目指す参照形）:
  - `オーバーレイのボタン入力 -> ゲームのカメラAPI呼び出し`
- 追加したい経路:
  - `VRコントローラーのスティック入力 -> ゲームのカメラAPI呼び出し`

つまり本質は、**入力デバイスを差し替えても、最終的に叩く先を同じカメラAPIに統一**する設計。

---

## 3. 現実的な採用方針（更新版）

## 採用方針: 「入力統合レイヤ」を作り、出力先をカメラAPIに一本化

- オーバーレイ入力もVRコントローラー入力も、
  まず共通の「CameraCommand（抽象コマンド）」へ変換
- 変換後は共通の「CameraApiExecutor」で、ゲーム側カメラAPIを呼ぶ
- `WASD/QERF` は**互換用（既存フォールバック）**として残しつつ、主経路にしない

### この方針のメリット

- 入力デバイスが増えても（将来はゲームパッド等）処理を使い回せる
- 操作感の調整（デッドゾーン、感度、加速度）をAPI呼び出し直前に一元管理できる
- オーバーレイとVRの挙動差分を減らせる

---

## 4. 具体設計（実装前メモ）

## 4.1 コマンドモデル（例）

入力を次のようなコマンドに落とし込む:

- `MoveForward(value)` / `MoveRight(value)` / `MoveUp(value)`
- `LookYaw(value)` / `LookPitch(value)`
- `ChangeFov(value)`
- `SwitchCameraMode()`
- `SwitchCameraSubMode()`
- `SelectPrevCharacter()` / `SelectNextCharacter()`

> ここでの value は -1.0〜1.0 などの正規化値。

## 4.2 実行先（ゲーム側カメラAPI）

既存のカメラ操作関数群（例: `camera_forward`, `camera_right`, `camera_up`, `cameraLookat_*`, `SwitchCameraMode` など）を
「共通実行先」として呼ぶ想定。

- オーバーレイ入力 -> CameraCommand化 -> カメラAPI実行
- VR入力 -> CameraCommand化 -> カメラAPI実行

この形に揃えると、要件どおり「VR入力を直接WASDへ落とさない」設計になる。

## 4.3 VRコントローラー（初期マッピング）

### 移動

- 左スティック Y -> `MoveForward(value)`
- 左スティック X -> `MoveRight(value)`

### 上下

- トリガー差分またはボタン -> `MoveUp(value)`

### 視点

- 右スティック X -> `LookYaw(value)`
- 右スティック Y -> `LookPitch(value)`

### モード切替

- 任意ボタン -> `SwitchCameraMode()`
- 任意ボタン長押し -> `SwitchCameraSubMode()`（必要なら）

### HMD

- 現段階では無効（無視）

---

## 5. BlueStacks + Quest 3 前提の段階目標（更新）

## Phase 0: 接続安定

- Quest 3表示とBlueStacks側アプリの接続を安定化

## Phase 1: API直叩き経路の成立

- オーバーレイ入力を CameraCommand 経由でカメラAPI呼び出しに統一
- 既存のキーイベント経路はフォールバックとして維持

## Phase 2: VR入力統合

- VRスティック入力を CameraCommand 化し、同じカメラAPIへ流す
- `WASD/QERF` への直接変換を主経路から外す

## Phase 3: 操作品質調整

- デッドゾーン、感度、加速度、更新レートを調整
- Fes Liveでの実使用（カメラ暴走や入力取りこぼし確認）

---

## 6. 今回の最終提案（短く）

要望どおり、方針は次で確定するのが良い。

- **「VRコントローラー入力 -> ゲーム側カメラAPI呼び出し」** を主経路にする
- オーバーレイ入力も同じ経路へ寄せ、入力統合レイヤを作る
- HMD頭部トラッキングは今は無視
- 映像は単眼両目表示で開始

これにより、操作系の責務が明確になり、将来の拡張（HMD連動やstereo化）も進めやすくなる。

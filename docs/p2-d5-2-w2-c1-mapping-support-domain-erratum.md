# P2-D5-2 W2-C1 Mapping Support Domain — Contract Erratum / Amendment

## 何が起きたか

W3 fresh acquisition (3 run × 60s) の C1 が不成立になった。原因は W3 が作った欠陥ではなく、
**C1.3 の observed-diagnostic 要求が mapping support より広い定義域を主張していた**ことである。
長尺 capture がそれを露出させた。

```text
run1  未map displayed_qpc = 227554590208   successor_qpc との差 = -16.691 ms
run2  未map displayed_qpc = 228889693293                        = -16.634 ms
run3  未map displayed_qpc = 230231634563                        = -16.622 ms
```

各 run の最後の 1 candidate が、physical mapping support の successor witness より
**ちょうど 1 refresh 分だけ後**に表示されていた。ETW capture envelope が mapping support より
長く開いていたため、対応する VBlank sample が存在しない時刻の Presented が 1 件だけ捕まる。

### これは元から race だった

fresh-7 (3 run × 5s) の同じ量を測ると、最後の candidate は successor の**わずか内側**だった。

```text
fresh-7   margin = +0.011ms / +0.060ms / +0.014ms   observed missing = 0
W3        margin = -16.691 / -16.634 / -16.622 ms   observed missing = 1
```

fresh-7 は 11〜60 マイクロ秒差で 3 回とも内側に落ちていただけである。60 秒化が原因ではない。
「capture envelope の閉じ」と「mapping support successor」の間に bracket が無いことが実体である。

## 何を直したか — A ではなく B

runtime / acquisition を変えて diagnostic tail を support 内へ押し込む (A) のではなく、
**mapping authority の定義域を exact support に一致させる contract correction** を採った。
producer / measurement semantics は変更していない。

### exact mapping support

mapping rule が解を持ちうる QPC 区間は support sample の閉区間に限られる。

```text
support           = samples[ordinal ∈ [predecessor_ordinal, successor_ordinal]]
mapping support   = [support.first.qpc, support.last.qpc]
```

この外側の DisplayedQPC には対応する VBlank witness が存在しない。そこまで
「mapping されなければ INVALID」と要求することは、support 外について authority を
主張することであり、それがこの race を作っていた。

### 新しい分類

```text
observed Presented candidate
  ├─ INSIDE_SUPPORT
  │    physical_vblank_mapping_required = true
  │    missing / ambiguous は INVALID
  │
  └─ BEFORE_PREDECESSOR / AFTER_SUCCESSOR
       physical_vblank_mapping_required = false
       mapping を要求しない。diagnostic として記録する
```

support 外を許すのは厳密に次の場合だけである。

```text
in_b2_formal_presented_population == false
AND
in_domain == false
```

formal Presented が support 外、または in-domain Presented が support 外なら
**authority INVALID のまま**である。

```text
FORMAL_PRESENTED_OUTSIDE_MAPPING_SUPPORT
IN_DOMAIN_PRESENTED_OUTSIDE_MAPPING_SUPPORT
```

formal 母集団の呼び出しには `RequireAllCandidatesInsideSupport = $true` を渡している。
したがって formal-v2 canonical chain の厳密性は一切下がっていない。

### 捨てずに記録する

artifact は次を持つ。

```text
mapping_support_domain = CLOSED_SUPPORT_SAMPLE_QPC_INTERVAL
observed_presented_count
observed_inside_mapping_support_count
observed_outside_mapping_support_count
outside_mapping_support_head_count
outside_mapping_support_tail_count
formal_outside_mapping_support_count      = 0 でなければ INVALID
in_domain_outside_mapping_support_count   = 0 でなければ INVALID
```

record 自体も残す。

```text
displayed_qpc
mapping_support_relation          = BEFORE_PREDECESSOR / INSIDE_SUPPORT / AFTER_SUCCESSOR
physical_vblank_mapping_required  = false
in_measurement_physical_domain    = false
```

contract は successor 側に特殊化していない。**support 外一般**として実装しているため、
将来の lower-boundary 問題にも同じ判定が効く。

## C1.4 は artifact の outside 判定を信用しない

sealed replay checker は、artifact が `outside_mapping_support = true` と書いているから
除外する、という形にしていない。C1.4 自身が sealed physical samples / predecessor /
successor から support を再構築し、`DisplayedQPC ∈ support ?` を独立に再計算して、
artifact の `mapping_support_relation` / `physical_vblank_mapping_required` /
outside counts と record 単位で一致させる。

これにより「本当は support 内の missing event を outside 扱いへ書き換える」false-PASS を防ぐ。

## 副次的に判明した構造

support 内では **`PHYSICAL_MAPPING_MISSING` は構造的に発生し得ない**。mapping cell は
`[first.qpc] ∪ (s[i-1].qpc, s[i].qpc]` で連続しているため、`[first, last]` の任意の QPC は
ちょうど 1 つの解を持つ。従来 missing が観測されていたのは support 外だけであり、
これが定義域のズレの実体だった。

旧 `NegativeNoPhysicalMapping` (displayed=50、predecessor より前) はこの性質を体現していた
fixture なので、両 disposition へ分解した。

```text
GoodHeadOutsideSupportNonFormal          non-formal / out-of-domain なら PASS
NegativeFormalPresentedOutsideSupportHead formal なら INVALID
```

## negative

`p2_d5_2_w2c1_displayed_mapping_*` (25 件) に次を追加した。

```text
GoodTailOutsideSupportNonFormal              support外tailはPASS
GoodHeadOutsideSupportNonFormal              support外headはPASS
GoodTailInsideSupportByMargin                successorちょうど内側はmapping requiredのままexact
NegativeFormalPresentedOutsideSupportTail    formalがsupport外ならINVALID
NegativeFormalPresentedOutsideSupportHead    同上 (head側)
NegativeInDomainPresentedOutsideSupport      in-domainがsupport外ならINVALID
NegativeInsideSupportMissingMappingMarkedOutside  support内をoutsideへ書き換えたらINVALID
NegativeOutsideSupportTailCountMutation      outside countの改変はINVALID
NegativeSuccessorMutationMakesCandidateInside successorを伸ばした再生はsealed supportと不一致
```

**11µs 内側と 16.6ms 外側を support 境界だけで機械的に区別する**ことを固定している。
capture 長や「最後の candidate だから」といった heuristic は使っていない。

## historical closure は書き換えない

fresh-7 は当時の contract でも実際に全 candidate が support 内に入り、C1.4 を正当に PASS して
いた。したがって次は INVALID へ変更しない。

```text
W2-C1  PASS / CLOSED @5034bfcd...
W2-D   PASS / CLOSED
W2-E   PASS / CLOSED
```

本 erratum は「旧 observed-diagnostic total-mapping requirement に latent な
support-domain mismatch があり、W3 が露出させた」という記録である。

### fresh-7 offline replay

修正版 C1.3 / C1.4 で fresh-7 を offline replay し、**formal 結果が完全に同一**であることを
確認した。

```text
formal aggregate 17 項目すべて一致 (presented / mapped / in_domain / verdict など)
observed_inside_mapping_support_count  = 895
observed_outside_mapping_support_count = 0      ← 11µs 差が内側だった裏付け
C1.4 checker: PASS (438/895 formal/observed)
```

## W3 cohort は再取得しない

W3 acquisition は binary / protocol 自体が成立しており、formal Presented は 5226/5226 exact
だった。producer / runtime / measurement semantics を変更していないため、**保存済みの同じ
sealed W3 cohort を offline で再評価**する。

修正版での W3 C1 結果:

```text
formal_presented_count                 5226 (1742 x 3 run)
mapped_exact_count                     5226
in_domain_presented_event_count        5226
observed_presented_count               6216
observed_inside_mapping_support_count  6213
observed_outside_mapping_support_count    3  (すべて tail)
formal_outside_mapping_support_count      0
in_domain_outside_mapping_support_count   0
verdict  DISPLAYED_QPC_PHYSICAL_VBLANK_ORDINAL_EXACT
```

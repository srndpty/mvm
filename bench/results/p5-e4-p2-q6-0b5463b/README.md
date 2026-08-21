# P5-E4 / P2-Q6 — Opportunity-Ordinal Scheduler Proof

DIAGNOSTIC_ONLY。Q5 historical evidenceとP2-D5-1 FAILは変更しない。
actual swap間隔をexact refresh rationalでopportunity ordinalへ変換し、60 fps source domainを
pure/offline replayした。production scheduler、formal checker、2% thresholdは変更していない。

全runでframe 0開始、unique frame strictly increasing、frame 3600非表示、
displayed + true_dropped = 3600を満たした。Q5のTRUE_OPPORTUNITY_LOSSとの差は、
cadence/domain lossとsynthetic skip地点以外のordinal gap lossへ分解して保存した。

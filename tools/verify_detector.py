"""Run the exact firmware detector on labeled synthetic PCM streams, without email."""
from pathlib import Path
import argparse
import json
import subprocess
import wave
import numpy as np

def main():
    p=argparse.ArgumentParser()
    p.add_argument('--exe',type=Path,required=True)
    p.add_argument('--work',type=Path,required=True)
    p.add_argument('--threshold',type=float,default=.91)
    args=p.parse_args()
    root=Path(__file__).resolve().parents[1]
    args.work.mkdir(parents=True,exist_ok=True)
    with wave.open(str(root/'assets/reference-16k.wav'),'rb') as w:
        signal=np.frombuffer(w.readframes(w.getnframes()),'<i2').astype(np.float64)/32768
    rate=16000
    rng=np.random.default_rng(20261020)
    def stream(cue,starts,length):
        x=np.zeros(round(length*rate))
        for sec in starts:
            offset=round(sec*rate)
            x[offset:offset+len(cue)]+=cue
        return x
    cases=[]
    cases.append(('reference_offset',stream(signal,[.137],7),[.137]))
    cases.append(('quiet_gain_0.1',stream(signal*.1,[1.03],7),[1.03]))
    cases.append(('loud_gain_2',stream(signal*2,[.951],7),[.951]))
    for snr in (20,10):
        noise=rng.normal(0,np.sqrt(np.mean(signal**2))/10**(snr/20),round(8*rate))
        cases.append((f'white_noise_snr_{snr}db',noise+stream(signal,[1.271],8),[1.271]))
    echo=np.zeros(len(signal)+5000)
    for delay,gain in [(0,1),(913,.35),(2471,.16)]:
        echo[delay:delay+len(signal)]+=gain*signal
    cases.append(('simulated_room_echo',stream(echo,[.417],8),[.417]))
    for speed in (.99,1.01):
        changed=np.interp(np.arange(0,len(signal),speed),np.arange(len(signal)),signal)
        cases.append((f'playback_speed_{speed}',stream(changed,[.323],8),[.323]))
    cases.append(('silence',np.zeros(10*rate),[]))
    cases.append(('random_noise',rng.normal(0,.06,10*rate),[]))
    cases.append(('reversed_reference',stream(signal[::-1],[.5],7),[]))
    tone=np.sin(2*np.pi*1000*np.arange(10*rate)/rate)*.1
    cases.append(('steady_1khz_tone',tone,[]))
    # Five minutes is not a stop condition: validate ten independent occurrences.
    starts=[1.173+60*i for i in range(10)]
    cases.append(('once_per_minute_ten_events',stream(signal,starts,548),starts))
    starts=[.233,4.033,7.833]
    cases.append(('three_plays_close_together',stream(signal,starts,13),starts))
    rows=[]
    for name,a,starts in cases:
        raw=args.work/f'{name}.pcm'
        raw.write_bytes(np.clip(np.rint(a*32768),-32768,32767).astype('<i2').tobytes())
        run=subprocess.run([str(args.exe.resolve()),str(raw.resolve()),str(args.threshold)],
                           check=True,capture_output=True,text=True)
        result=json.loads(run.stdout)
        # Windows must align to the template; permit a 0.5s boundary tolerance.
        timing=len(result['events'])==len(starts) and all(
            2.8<=e['seconds']-s<=4.3 for e,s in zip(result['events'],starts))
        row=dict(case=name,expected=len(starts),passed=timing,**result)
        rows.append(row)
        print(name, 'PASS' if timing else 'FAIL', result['count'], 'max',result['max_score'])
    report=dict(threshold=args.threshold,cases=rows,passed=all(r['passed'] for r in rows),
        limitations='Synthetic tests only. No actual loudspeaker, room noise, microphone, Wi-Fi, or inbox delivery tested.')
    (root/'verification.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
    return 0 if report['passed'] else 1

if __name__=='__main__':
    raise SystemExit(main())

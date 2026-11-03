"""Post-interruption question-gate source guards and local output timing models. C/device acceptance runs separately."""
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / 'firmware/main'
acoustic = (MAIN / 'app_acoustic.c').read_text()
audio = (MAIN / 'app_audio.c').read_text()
runner = (MAIN / 'main.c').read_text()
tts = (MAIN / 'app_cloud_tts.c').read_text()
assert 'if (!pending_question) n = 0;' in acoustic
assert 'pending_voice_run >= RATE / 5' in acoustic
assert 'capture_finished = pending_question &&' in acoustic
assert 'if (interrupted_capture) wait_for_speech = true;' in audio
assert 'followup || interrupted);' in runner
assert 'if ((followup || interrupted) && result == ESP_ERR_NOT_FOUND' in runner
assert 'if (tail_guard)' in audio
assert audio.index('if (tail_guard)') < audio.index('if (progress && !waiting)')
for metric in ('s_convert_max_us', 's_reference_max_us', 's_i2s_max_us'):
    assert metric in audio
assert 'app_audio_playback_diagnostics(true)' in tts
assert 'app_audio_playback_diagnostics(false)' in tts

def gate(frames):
    run = 0
    confirmed = False
    for echoing, speech in frames:
        if echoing:
            run = 0
        else:
            run = run + 20 if speech else 0
            if run >= 200:
                confirmed = True
    return confirmed

assert not gate([(True, True)] * 100 + [(False, False)] * 60)
assert not gate([(False, True)] * 4 + [(False, False)] * 60)
assert gate([(True, True)] * 20 + [(False, True)] * 10)
assert gate([(False, True)] * 10 + [(False, False)] * 60)
assert not gate([(False, True), (False, False)] * 50)
print('PASS: post-playback speech required, quiet interruption waits locally, output timing guards; scope=offline checks; pending=C/device validation')

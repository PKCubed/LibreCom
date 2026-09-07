import os
import sys
import subprocess
import argparse
import shutil

def check_ffmpeg():
    """Check if ffmpeg is installed and accessible in the system PATH."""
    try:
        subprocess.run(['ffmpeg', '-version'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True)
    except (subprocess.CalledProcessError, FileNotFoundError):
        print("Error: 'ffmpeg' is not installed or not found in PATH.")
        print("Please install ffmpeg with support for libopus, libopencore_amrnb, and libcodec2.")
        sys.exit(1)

def run_cmd(cmd, step_name):
    """Helper to run a subprocess command and handle errors neatly."""
    try:
        subprocess.run(cmd, capture_output=True, check=True)
    except subprocess.CalledProcessError as e:
        print(f"Failed during {step_name}. Error:")
        print(e.stderr.decode('utf-8', errors='ignore'))
        return False
    return True

def clean_directory(directory):
    """Recursively delete all contents of a directory without removing the root directory itself."""
    if os.path.exists(directory):
        for filename in os.listdir(directory):
            file_path = os.path.join(directory, filename)
            if os.path.isfile(file_path):
                try:
                    os.remove(file_path)
                except Exception:
                    pass
            elif os.path.isdir(file_path):
                shutil.rmtree(file_path, ignore_errors=True)

def compress_audio(input_files, output_dir="compressed", mix_name="mixed", music_mode=False):
    if not (1 <= len(input_files) <= 4):
        print("Error: Please provide between 1 and 4 input files.")
        sys.exit(1)
        
    for f in input_files:
        if not os.path.isfile(f):
            print(f"Error: Input file '{f}' not found.")
            sys.exit(1)

    # Clean output directory
    print(f"Cleaning existing files in output directory: {os.path.abspath(output_dir)}")
    clean_directory(output_dir)
    os.makedirs(output_dir, exist_ok=True)

    # Create temp directory for normalized and intermediate files
    temp_dir = os.path.join(output_dir, 'temp')
    os.makedirs(temp_dir, exist_ok=True)

    # 0. Normalize audio files before any compression
    print("\nNormalizing input files...")
    normalized_files = []
    for idx, f in enumerate(input_files, 1):
        norm_file = os.path.join(temp_dir, f"norm_f{idx}.wav")
        # loudnorm applies EBU R128 loudness normalization. Force 48kHz output to prevent 192kHz resampling bugs.
        cmd_norm = ['ffmpeg', '-y', '-i', f, '-af', 'loudnorm', '-ar', '48000', norm_file]
        if run_cmd(cmd_norm, f"Normalizing {f}"):
            normalized_files.append(norm_file)
        else:
            print(f"Failed to normalize {f}. Exiting.")
            sys.exit(1)

    # Build a list of encoding tasks
    tasks = []

    if music_mode:
        # Music Opus only (from 16k up to 256k)
        for br_label, br_val in [('16k', 16000), ('24k', 24000), ('32k', 32000), ('48k', 48000), 
                                 ('64k', 64000), ('96k', 96000), ('128k', 128000), ('192k', 192000), ('256k', 256000)]:
            tasks.append({
                'name': 'opus_music', 'br_label': br_label, 'br_val': br_val, 'ext': 'opus',
                'enc_args': ['-c:a', 'libopus', '-b:a', str(br_val)]
            })
    else:
        # 1. Opus
        for br_label, br_val in [('8k', 8000), ('12k', 12000), ('16k', 16000), ('32k', 32000)]:
            tasks.append({
                'name': 'opus', 'br_label': br_label, 'br_val': br_val, 'ext': 'opus',
                'enc_args': ['-c:a', 'libopus', '-b:a', str(br_val)]
            })

        # 2. AMR-NB
        for br_label, br_val in [('4.75k', 4750), ('7.4k', 7400), ('12.2k', 12200)]:
            tasks.append({
                'name': 'amrnb', 'br_label': br_label, 'br_val': br_val, 'ext': 'amr',
                'enc_args': ['-c:a', 'libopencore_amrnb', '-ar', '8000', '-ac', '1', '-b:a', str(br_val)]
            })

        # 3. Codec 2
        for mode_label, br_val in [('1200', 1200), ('2400', 2400), ('3200', 3200)]:
            tasks.append({
                'name': 'codec2', 'br_label': mode_label, 'br_val': br_val, 'ext': 'c2',
                'enc_args': ['-c:a', 'libcodec2', '-ar', '8000', '-ac', '1', '-mode', mode_label]
            })

        # 4. ADPCM (IMA WAV)
        for sr, br_label, br_val in [('8000', '32k', 32000), ('16000', '64k', 64000), ('32000', '128k', 128000)]:
            tasks.append({
                'name': 'adpcm', 'br_label': br_label, 'br_val': br_val, 'ext': 'wav',
                'enc_args': ['-c:a', 'adpcm_ima_wav', '-ar', sr, '-ac', '1']
            })

    # Sort tasks by numeric bitrate (br_val)
    tasks.sort(key=lambda x: x['br_val'])

    # Determine mode based on number of input files
    num_files = len(normalized_files)
    mode = "MIX" if num_files > 1 else "SINGLE"
    print(f"\nEncoding {len(tasks)} configurations in order of bitrate (Mode: {mode})...\n")

    for i, task in enumerate(tasks, 1):
        prefix = f"{i:02d}"
        
        if mode == "SINGLE":
            comp_file = os.path.join(output_dir, f"{prefix}_{task['name']}_{task['br_label']}.{task['ext']}")
            dec_file = os.path.join(output_dir, f"{prefix}_{task['name']}_{task['br_label']}_decoded.wav")
            
            print(f"[{prefix}/{len(tasks):02d}] {task['name'].upper()} @ {task['br_label']} bps")
            cmd_enc = ['ffmpeg', '-y', '-i', normalized_files[0]] + task['enc_args'] + [comp_file]
            if run_cmd(cmd_enc, f"{task['name']} encoding ({task['br_label']})"):
                cmd_dec = ['ffmpeg', '-y', '-i', comp_file, '-c:a', 'pcm_s16le', dec_file]
                run_cmd(cmd_dec, f"{task['name']} decoding ({task['br_label']})")
                
        else: # MIX mode (2 to 4 files)
            print(f"[{prefix}/{len(tasks):02d}] {task['name'].upper()} @ {task['br_label']} bps (Mixing {num_files} files)")
            
            dec_files = []
            success = True
            
            for idx, in_file in enumerate(normalized_files, 1):
                comp_file = os.path.join(temp_dir, f"f{idx}_{task['name']}_{task['br_label']}.{task['ext']}")
                dec_file = os.path.join(temp_dir, f"f{idx}_{task['name']}_{task['br_label']}_decoded.wav")
                
                cmd_enc = ['ffmpeg', '-y', '-i', in_file] + task['enc_args'] + [comp_file]
                cmd_dec = ['ffmpeg', '-y', '-i', comp_file, '-c:a', 'pcm_s16le', dec_file]
                
                if run_cmd(cmd_enc, f"F{idx} {task['name']} enc") and run_cmd(cmd_dec, f"F{idx} {task['name']} dec"):
                    dec_files.append(dec_file)
                else:
                    success = False
                    break
            
            # If all files successfully encoded and decoded, mix them together
            if success and len(dec_files) == num_files:
                final_mixed = os.path.join(output_dir, f"{prefix}_{task['name']}_{task['br_label']}_{mix_name}.wav")
                
                # Build the ffmpeg mix command dynamically based on the number of files
                cmd_mix = ['ffmpeg', '-y']
                for df in dec_files:
                    cmd_mix.extend(['-i', df])
                
                cmd_mix.extend(['-filter_complex', f'amix=inputs={num_files}:duration=longest', final_mixed])
                
                run_cmd(cmd_mix, f"Mixing {task['name']} ({task['br_label']})")

    # Clean up all intermediate files (including normalized inputs)
    if os.path.exists(temp_dir):
        shutil.rmtree(temp_dir, ignore_errors=True)
            
    print("\nFinished processing all algorithms!")

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Normalize, compress, decode, and optionally mix up to 4 audio files.")
    parser.add_argument("input_files", nargs='+', help="Path to 1 to 4 input audio files")
    parser.add_argument("--outdir", default="compressed", help="Output directory")
    parser.add_argument("--mix-name", default="mixed", help="Custom name for the combined output files (e.g., dual-voice)")
    parser.add_argument("--music", action="store_true", help="Enable music mode (tests only Opus from 16kbps up to 256kbps)")
    
    args = parser.parse_args()
    check_ffmpeg()
    compress_audio(args.input_files, args.outdir, args.mix_name, args.music)

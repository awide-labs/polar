#!/usr/bin/env python3

# generate-version-badge.py
#
# Copyright (c) 2025, Alibaba Group Holding Limited
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# IDENTIFICATION
#	  polar-doc/docs/zh/features/generate-version-badge.py

import os
import re

def get_title_from_md(file_path):
    with open(file_path, 'r', encoding='utf-8') as file:
        for line in file:
            match = re.match(r'# (.+)', line)
            if match:
                return match.group(1)
    return None

def get_badge_lines(file_path):
    badge_lines = []
    with open(file_path, 'r', encoding='utf-8') as file:
        for line in file:
            if line.strip().startswith('<Badge type="tip" text='):
                badge_lines.append(line.strip())
    return sorted(badge_lines, reverse=True)  # Sort badges in reverse order

def process_directory(directory, base_path='.'):
    md_files = [f for f in os.listdir(directory) if f.endswith('.md') and f != 'README.md']
    md_files.sort()  # Sort by file name
    links = []

    for md_file in md_files:
        file_path = os.path.join(directory, md_file)
        title = get_title_from_md(file_path)
        if title:
            badges = get_badge_lines(file_path)
            badge_text = ' '.join(badges)
            relative_path = os.path.relpath(file_path, base_path)
            links.append(f'- [{title}](./{relative_path}) {badge_text}'.strip())

    return links

def generate_version_badge(directories):
    for current_dir in directories:
        all_links = []
        readme_path = os.path.join(current_dir, 'README.md')

        # Process current directory
        all_links.extend(process_directory(current_dir, current_dir))

        # Process subdirectories
        subdirs = sorted([d for d in os.listdir(current_dir) if os.path.isdir(os.path.join(current_dir, d))])
        for subdir in subdirs:
            subdir_path = os.path.join(current_dir, subdir)
            subdir_readme_path = os.path.join(subdir_path, 'README.md')
            if os.path.exists(subdir_readme_path):
                subdir_title = get_title_from_md(subdir_readme_path)
                if subdir_title:
                    all_links.append(f'\n## {subdir_title}\n')
                    subdir_links = process_directory(subdir_path, current_dir)
                    all_links.extend(subdir_links)

        with open(readme_path, 'r', encoding='utf-8') as readme_file:
            lines = readme_file.readlines()

        with open(readme_path, 'w', encoding='utf-8') as readme_file:
            readme_file.writelines(lines[:2])  # Keep the first two lines
            readme_file.write('\n'.join(all_links).strip() + '\n')  # Write the new links list

if __name__ == '__main__':
    script_dir = os.path.dirname(os.path.abspath(__file__))  # Get the directory of the script file
    exclude_dirs = ['release-notes', 'another-dir-to-exclude']  # List of directories to exclude
    directories = [os.path.join(script_dir, d) for d in os.listdir(script_dir) if os.path.isdir(os.path.join(script_dir, d)) and d not in exclude_dirs]
    generate_version_badge(directories)

#!/usr/bin/env python3

# generate-index.py
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
#	  polar-doc/docs/zh/features/generate-index.py

import os
import re

def adjust_headings(content, subdir):
    """Adjust the headings in the content by increasing their level by two and fix links."""
    adjusted_lines = []
    for line in content.splitlines():
        if line.startswith('#'):
            line = '##' + line
        # Fix links to include subdir
        line = re.sub(r'\]\(\./', f'](./{subdir}/', line)
        adjusted_lines.append(line)
    return '\n'.join(adjusted_lines) + '\n'  # Ensure each content ends with a newline

def extract_readme_content(directory, subdir):
    """Extract the content of README.md from the specified directory."""
    readme_path = os.path.join(directory, 'README.md')
    if os.path.exists(readme_path):
        with open(readme_path, 'r', encoding='utf-8') as file:
            content = file.read()
            return adjust_headings(content, subdir)
    return ''

def inject_content_into_main_readme(main_readme_path, content_to_inject):
    """Inject the content into the '功能导览' section of the main README.md."""
    with open(main_readme_path, 'r', encoding='utf-8') as file:
        lines = file.readlines()

    new_lines = []
    in_functional_guide_section = False

    for line in lines:
        if line.strip() == '## 功能导览':
            new_lines.append(line)
            new_lines.append('\n')  # Add an empty line after '功能导览'
            new_lines.append(content_to_inject)
            in_functional_guide_section = True
        elif in_functional_guide_section and line.startswith('## '):
            in_functional_guide_section = False
            new_lines.append(line)
        elif not in_functional_guide_section:
            new_lines.append(line)

    with open(main_readme_path, 'w', encoding='utf-8') as file:
        file.write(''.join(new_lines))

def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))  # Get the directory of the script file
    exclude_dirs = ['release-notes', 'architecture']  # List of directories to exclude
    directories = sorted([d for d in os.listdir(script_dir) if os.path.isdir(os.path.join(script_dir, d)) and d not in exclude_dirs])

    content_to_inject = ''
    for directory in directories:
        content_to_inject += extract_readme_content(os.path.join(script_dir, directory), directory) + '\n'

    main_readme_path = os.path.join(script_dir, 'README.md')
    inject_content_into_main_readme(main_readme_path, content_to_inject.strip() + '\n')

if __name__ == '__main__':
    main()

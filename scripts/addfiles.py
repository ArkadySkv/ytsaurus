#!/usr/bin/python
################################################################################

import os;

UNITTEST_PREFIX = 'unittest'
MISC_PROJECT = 'misc'
HEADER_EXTENSION = '.h'
SOURCE_EXTENSION = '.cpp'
INLINE_EXTENSION = '-inl.h'
UNITTEST_EXTENSION = '_ut.cpp'
EXTENSIONS = [HEADER_EXTENSION, SOURCE_EXTENSION, INLINE_EXTENSION, UNITTEST_EXTENSION]


def get_cmake_omm_path():
    return os.path.abspath(os.path.join(os.path.dirname(__file__), 'cmake-omm.py'))


def get_ytlib_path():
    return os.path.abspath(os.path.join(os.path.dirname(__file__), '../yt/ytlib'))


def get_project_path(project_name):
    return os.path.join(get_ytlib_path(), project_name)


def get_unittests_path():
    return os.path.abspath(os.path.join(get_ytlib_path(), '../unittests'))


def add_file(file_path, file_data):
    if not os.path.exists(file_path):
        with open(file_path, 'w') as file:
            file.write(file_data)
    
    os.chdir(os.path.dirname(file_path))
    os.system('git add ' + os.path.basename(file_path))

    print file_path, 'added.'


def append_to_header_file(file_path, file_name):
    with open(file_path, 'r') as file:
        text = file.read()
    
    appendix = '''
#define {0}
#include "{1}"
#undef {0}
'''.format(get_inl_define(file_name), file_name + INLINE_EXTENSION)
    
    if not text.strip().endswith(appendix.strip()):
        text += appendix
    
    with open(file_path, 'w') as file:
        file.write(text)


def get_namespace(project_name):
    result = 'N'
    up = True
    for c in project_name:
        if c == '_':
            up = True
        else:
            result += c.upper() if up else c
            up = False
    return result


def get_inl_define(file_name):
    return (file_name + INLINE_EXTENSION + '_').replace('.', '_').replace('-', '_').upper()


def get_common_data(project_name):
    data = '''
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
'''
    
    if project_name != MISC_PROJECT:
        data = '''namespace {0} {{
{1}            
}} // namespace {0}'''.format(get_namespace(project_name), data)
    
    data = '''
namespace NYT {{
{0}
}} // namespace NYT
'''.format(data)

    return data


def get_header_data(project_name):
    return '''#pragma once
''' + get_common_data(project_name)


def get_source_data(project_name, file_name):
    return '''#include "stdafx.h"
#include "{0}"
'''.fomat(file_name + INLINE_EXTENSION) + get_common_data(project_name)


def get_inline_data(project_name, file_name):
    return '''#ifndef {0}
#error "Direct inclusion of this file is not allowed, include {1}"
#endif
#undef {0}
'''.format(get_inl_define(file_name), file_name + HEADER_EXTENSION) +
        get_common_data()


def get_unittest_data(project_name, file_name):
    return '''#include "stdafx.h"

#include <ytlib/{0}/{1}>

#include <contrib/testing/framework.h>

namespace NYT {{

////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
    
}} // namespace NYT
'''.format(project_name, file_name + HEADER_EXTENSION)


def get_file_data(extension, project_name, file_name):
    if extension == HEADER_EXTENSION:
        return get_header_data(project_name)
    elif extension == SOURCE_EXTENSION:
        return get_source_data(project_name, file_name)
    elif extension == INLINE_EXTENSION:
        return get_inline_data(project_name, file_name)
    elif extension == UNITTEST_EXTENSION:
        return get_unittest_data(project_name, file_name)
    raise RuntimeError("Unknown extension {0}".format(extension))

def add_files(project_name, file_name, extensions):
    for extension in extensions:
        if extension == UNITTEST_EXTENSION:
            dir_name = get_unittests_path()
        else:
            dir_name = get_project_path(project_name)
        file_path = os.path.join(dir_name, file_name + extension)
        file_data = get_file_data(extension, project_name, file_name)
        add_file(file_path, file_data)
        if extension == HEADER_EXTENSION and INLINE_EXTENSION in extensions:
            append_to_header_file(file_path, file_name)

    os.system(get_cmake_omm_path() + ' --update')


def normalize(project_name, file_name, extensions):
    for extension in EXTENSIONS:
        if file_name.endswith(extension):
            extensions = [extension]
            file_name = file_name[:-len(extension)]
            break
    if project_name.startswith(UNITTEST_PREFIX):
        extensions = [UNITTEST_EXTENSION]
    if INLINE_EXTENSION in extensions and HEADER_EXTENSION not in extensions:
        extensions.append(HEADER_EXTENSION)
    return (file_name, extensions)


def print_usage():
    print 'Usage:   addfiles.py <dir_name> <file_name> [<extensions comma-separated>]'


if __name__ == '__main__':
    args = sys.argv
    if len(args) < 3 or len(args) > 4:
        print_usage()
        raise RuntimeError('Incorrect usage')
    project_name = args[1]
    file_name = args[2]
    extensions = [] if len(args) == 3 else args[3].split(',')
    (file_name, extensions) = normalize(project_name, file_name, extensions)

    add_files(project_name, file_name, extensions)

    print "Done."
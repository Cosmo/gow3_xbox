"""Fetch only pinned Boost headers needed by the existing renderer, into .local.

Archives stay in memory; tests, documentation and Git histories are not extracted.
"""
import io
import pathlib
import re
import tarfile
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parents[1] / '.local/deps/boost'
TAG = 'boost-1.88.0'
TOP = {
    'assert.hpp': 'assert', 'current_function.hpp': 'assert', 'config.hpp': 'config',
    'cstdint.hpp': 'config', 'version.hpp': 'config', 'limits.hpp': 'config',
    'throw_exception.hpp': 'throw_exception', 'noncopyable.hpp': 'core',
    'ref.hpp': 'core', 'get_pointer.hpp': 'core', 'type.hpp': 'core',
    'call_traits.hpp': 'type_traits', 'static_assert.hpp': 'static_assert',
    'operators.hpp': 'utility', 'next_prior.hpp': 'iterator', 'cast.hpp': 'conversion',
    'integer.hpp': 'integer', 'integer_traits.hpp': 'integer',
    'shared_ptr.hpp': 'smart_ptr', 'make_shared.hpp': 'smart_ptr',
    'scoped_ptr.hpp': 'smart_ptr', 'intrusive_ptr.hpp': 'smart_ptr',
    'enable_shared_from_this.hpp': 'smart_ptr', 'weak_ptr.hpp': 'smart_ptr',
    'type_index.hpp': 'type_index', 'optional.hpp': 'optional',
    'function.hpp': 'function', 'bind.hpp': 'bind', 'tuple.hpp': 'tuple',
    'concept_check.hpp': 'concept_check', 'concept': 'concept_check',
    'concept_archetype.hpp': 'concept_check', 'rational.hpp': 'rational',
    'lexical_cast.hpp': 'lexical_cast', 'array.hpp': 'array',
    'iterator_adaptors.hpp': 'iterator', 'visit_each.hpp': 'core',
    'detail': 'core',
}

def fetch(module):
    marker = ROOT / 'licenses' / (module + '.txt')
    if marker.exists():
        return
    url = f'https://codeload.github.com/boostorg/{module}/tar.gz/refs/tags/{TAG}'
    print(f'Fetching pinned headers: {module}', flush=True)
    request = urllib.request.Request(url, headers={'User-Agent': 'Gow3-UWP-build'})
    with urllib.request.urlopen(request, timeout=45) as response:
        data = response.read(24 * 1024 * 1024 + 1)
    if len(data) > 24 * 1024 * 1024:
        raise RuntimeError('Dependency archive exceeds the bounded download size')
    count = 0
    with tarfile.open(fileobj=io.BytesIO(data), mode='r:gz') as archive:
        license_text = None
        for entry in archive:
            if not entry.isfile():
                continue
            parts = pathlib.PurePosixPath(entry.name).parts[1:]
            if not parts or '..' in parts:
                continue
            if parts[0].startswith('LICENSE'):
                license_text = archive.extractfile(entry).read()
            if parts[:2] != ('include', 'boost'):
                continue
            destination = ROOT / pathlib.Path(*parts)
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(archive.extractfile(entry).read())
            count += 1
        if license_text is None:
            with urllib.request.urlopen(f'https://raw.githubusercontent.com/boostorg/boost/{TAG}/LICENSE_1_0.txt', timeout=30) as response:
                license_text = response.read()
        marker.parent.mkdir(parents=True, exist_ok=True)
        marker.write_bytes(license_text)
    print(f'{module}: {count} headers, archive {len(data) // 1024} KiB', flush=True)

def main():
    robin = ROOT.parent / 'robin-map'
    if not (robin / 'LICENSE').exists():
        with urllib.request.urlopen('https://codeload.github.com/Tessil/robin-map/tar.gz/refs/tags/v1.3.0', timeout=45) as response:
            data = response.read(2 * 1024 * 1024 + 1)
        if len(data) > 2 * 1024 * 1024:
            raise RuntimeError('robin-map archive exceeds bounded download size')
        with tarfile.open(fileobj=io.BytesIO(data), mode='r:gz') as archive:
            for entry in archive:
                parts = pathlib.PurePosixPath(entry.name).parts[1:]
                if entry.isfile() and parts and '..' not in parts and (parts[0] == 'include' or parts[0] == 'LICENSE'):
                    destination = robin.joinpath(*parts)
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    destination.write_bytes(archive.extractfile(entry).read())
    with urllib.request.urlopen(f'https://raw.githubusercontent.com/boostorg/boost/{TAG}/.gitmodules', timeout=30) as response:
        modules = set(re.findall(r'url = \.\./([\w-]+)\.git', response.read().decode()))
    needed = {'asio', 'container', 'container_hash', 'icl', 'pool', 'preprocessor',
              'config', 'core', 'assert', 'type_traits', 'move', 'intrusive', 'utility',
              'smart_ptr', 'static_assert', 'throw_exception', 'integer'}
    done = set()
    while needed - done:
        for module in sorted(needed - done):
            fetch(module)
            done.add(module)
        for header in (ROOT / 'include/boost').rglob('*'):
            if not header.is_file():
                continue
            text = header.read_text(errors='ignore')
            for include in re.findall(r'#\s*include\s*[<"]boost/([^>"\s]+)', text):
                module = TOP.get(include, include.split('/')[0])
                if include.startswith('numeric/conversion/'):
                    module = 'numeric_conversion'
                if module in modules:
                    needed.add(module)
    print(f'Pinned renderer headers ready: {len(done)} Boost modules', flush=True)

if __name__ == '__main__':
    main()

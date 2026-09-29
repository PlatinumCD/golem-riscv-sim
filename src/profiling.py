"""Optional cycle profiling; these settings never change hardware parameters."""
import os
from pathlib import Path


def enabled():
    flag = os.environ.get('TILE_CYCLE_PROFILE', '0')
    if flag not in ('0', '1'):
        raise ValueError('TILE_CYCLE_PROFILE must be 0 or 1')
    return flag == '1'


def configure(*, enabled=False, output_directory=None):
    """Set profiling before constructing SST components. Off is the default.

    An explicit directory is useful for standalone SST simulations. Test runs
    otherwise place state traces in each TILE_COMPONENT_OUTPUT/profiles folder.
    """
    if type(enabled) is not bool:
        raise ValueError('Profiling enabled must be boolean')
    path = None
    if enabled:
        if output_directory is None:
            base = os.environ.get('TILE_COMPONENT_OUTPUT')
            if not base:
                raise ValueError('Enabled profiling requires an output directory')
            path = Path(base) / 'profiles'
        else:
            if not str(output_directory):
                raise ValueError('Profiling output directory must not be empty')
            path = Path(output_directory)
        path = path.resolve()
        path.mkdir(parents=True, exist_ok=True)
    os.environ['TILE_CYCLE_PROFILE'] = '1' if enabled else '0'
    if path is None:
        os.environ.pop('TILE_CYCLE_PROFILE_DIRECTORY', None)
    else:
        os.environ['TILE_CYCLE_PROFILE_DIRECTORY'] = str(path)
    return path

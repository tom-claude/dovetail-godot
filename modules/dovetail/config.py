# Dovetail app bridge: editor binary only, never in export templates.


def can_build(env, platform):
    return env.editor_build


def configure(env):
    pass

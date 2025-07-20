import yaml
from pathlib import Path

def load_config():
    # Path to the default config (inside the project)
    default_config_path = Path(__file__).parent / "config.yaml"
    
    # Path to the user's override config (~/.config/aember/config.yaml)
    user_config_path = Path.home() / ".config/aember/config.yaml"
    
    # Load default config
    with open(default_config_path, "r") as f:
        config = yaml.safe_load(f) or {}

    # If user override exists, merge it
    if user_config_path.exists():
        with open(user_config_path, "r") as f:
            user_config = yaml.safe_load(f) or {}
            config.update(user_config)  # Override defaults
    
    return config

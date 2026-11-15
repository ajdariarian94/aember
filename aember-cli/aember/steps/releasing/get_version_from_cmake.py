import re


def get_version_from_cmake(cmake_file: str) -> str:
    """
    Extracts the project version from a CMakeLists.txt file.
    Handles multi-line project() definitions.
    """
    with open(cmake_file, "r") as f:
        content = f.read()

    # Remove comments
    content = re.sub(r"#.*", "", content)

    # Match project(... VERSION X.Y.Z ...) across multiple lines
    match = re.search(
        r"project\s*\(.*?VERSION\s*([0-9]+\.[0-9]+\.[0-9]+).*?\)",
        content,
        re.DOTALL | re.IGNORECASE,
    )

    if match:
        return match.group(1)
    raise ValueError("Version not found in CMakeLists.txt")

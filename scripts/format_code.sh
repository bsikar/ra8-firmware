#!/bin/bash
# ra8d2-firmware Code Formatting Script for Mac/Linux
# Usage: ./scripts/format_code.sh [options]

set -e  # Exit on any error
set +H  # Disable history expansion (fixes ! in if statements)

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Default values
CHECK_ONLY=false
VERBOSE=false
EXTENSIONS=("*.c" "*.h" "*.cpp" "*.hpp")
DIRECTORIES=("src" "libs" "tests")

# Print usage information
usage() {
    echo "ra8d2-firmware Code Formatting Script"
    echo ""
    echo "Usage: $0 [options]"
    echo ""
    echo "Options:"
    echo "  -c, --check    Check formatting without making changes"
    echo "  -v, --verbose  Enable verbose output"
    echo "  -h, --help     Show this help message"
    echo ""
    echo "Examples:"
    echo "  $0             # Format all source files"
    echo "  $0 --check     # Check formatting without changes"
    echo "  $0 -v          # Format with verbose output"
}

# Print colored output
print_status()  { echo -e "${BLUE}[INFO]${NC} $1" >&2; }
print_success() { echo -e "${GREEN}[SUCCESS]${NC} $1" >&2; }
print_warning() { echo -e "${YELLOW}[WARNING]${NC} $1" >&2; }
print_error()   { echo -e "${RED}[ERROR]${NC} $1" >&2; }

# Check if clang-format is installed
check_clang_format() {
    if ! command -v clang-format &> /dev/null; then
        print_error "clang-format not found!"
        echo ""
        echo "Please install clang-format:"
        echo "  macOS: brew install clang-format"
        echo "  Ubuntu/Debian: sudo apt-get install clang-format"
        echo "  Fedora: sudo dnf install clang"
        exit 1
    fi

    local version
    version=$(clang-format --version | head -n1)
    if [ "$VERBOSE" = true ]; then
        print_status "Found $version"
    fi
}

# Check if .clang-format file exists
check_clang_format_config() {
    if [ ! -f ".clang-format" ]; then
        print_error ".clang-format configuration file not found!"
        echo "Please ensure you're running this script from the project root directory."
        exit 1
    fi

    if [ "$VERBOSE" = true ]; then
        print_status "Found .clang-format configuration"
    fi
}

# Parse command line arguments
parse_args() {
    while [[ $# -gt 0 ]]; do
        case $1 in
            -c|--check)
                CHECK_ONLY=true
                shift
                ;;
            -v|--verbose)
                VERBOSE=true
                shift
                ;;
            -h|--help)
                usage
                exit 0
                ;;
            *)
                print_error "Unknown option: $1"
                usage
                exit 1
                ;;
        esac
    done
}

# Find all source files
find_source_files() {
    local files=()

    for dir in "${DIRECTORIES[@]}"; do
        if [ ! -d "$dir" ]; then
            if [ "$VERBOSE" = true ]; then
                print_warning "Directory '$dir' not found, skipping..."
            fi
            continue
        fi

        for ext in "${EXTENSIONS[@]}"; do
            while IFS= read -r -d '' file; do
                files+=("$file")
            done < <(find "$dir" -name "$ext" -type f \
                -not -path "*/build/*" \
                -not -path "*/_deps/*" \
                -print0 2>/dev/null)
        done
    done

    printf '%s\n' "${files[@]}"
}

# Check formatting of files
check_formatting() {
    local files=("$@")
    local issues_found=false

    print_status "Checking code formatting..."

    for file in "${files[@]}"; do
        if [ "$VERBOSE" = true ]; then
            echo "  Checking: $file" >&2
        fi

        if ! clang-format --dry-run --Werror "$file" >/dev/null 2>&1; then
            if [ "$issues_found" = false ]; then
                echo "" >&2
                print_warning "Formatting issues found in:"
                issues_found=true
            fi
            echo "  $file" >&2
        fi
    done

    if [ "$issues_found" = true ]; then
        echo "" >&2
        print_error "Code formatting check failed!"
        echo "Run 'bash scripts/format_code.sh' to fix formatting issues." >&2
        return 1
    else
        print_success "All files are properly formatted!"
        return 0
    fi
}

# Format files
format_files() {
    local files=("$@")
    local formatted_count=0

    print_status "Formatting source files..."

    for file in "${files[@]}"; do
        if [ "$VERBOSE" = true ]; then
            echo "  Processing: $file" >&2
        fi

        local temp_file
        temp_file=$(mktemp)

        clang-format "$file" > "$temp_file" 2>&1 || {
            echo "ERROR: clang-format failed on $file" >&2
            rm "$temp_file"
            continue
        }

        if ! cmp -s "$file" "$temp_file" 2>/dev/null; then
            cp "$temp_file" "$file"
            ((formatted_count++)) || true
            if [ "$VERBOSE" = true ]; then
                echo "    [PASS] Formatted" >&2
            fi
        elif [ "$VERBOSE" = true ]; then
            echo "    - No changes needed" >&2
        fi

        rm "$temp_file"
    done

    if [ $formatted_count -gt 0 ]; then
        print_success "Formatted $formatted_count file(s)!"
    else
        print_success "All files were already properly formatted!"
    fi
}

# Main execution
main() {
    # Change to project root directory
    cd "$(dirname "$0")/.."

    parse_args "$@"

    check_clang_format
    check_clang_format_config

    print_status "Searching for source files in: ${DIRECTORIES[*]}"
    IFS=$'\n' read -d '' -r -a source_files < <(find_source_files && printf '\0')

    if [ ${#source_files[@]} -eq 0 ]; then
        print_warning "No source files found!"
        exit 0
    fi

    if [ "$VERBOSE" = true ]; then
        print_status "Found ${#source_files[@]} source file(s)"
    fi

    if [ "$CHECK_ONLY" = true ]; then
        check_formatting "${source_files[@]}"
    else
        format_files "${source_files[@]}"
    fi
}

main "$@"

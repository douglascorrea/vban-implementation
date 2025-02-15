#!/bin/bash

# Function to convert stream name to slug
slugify() {
    echo "$1" | iconv -t ascii//TRANSLIT | sed -E 's/[^a-zA-Z0-9]+/-/g' | sed -E 's/^-+|-+$//g' | tr '[:upper:]' '[:lower:]'
}

# Function to get port from config file
get_port() {
    local config_file="$1"
    grep "port=" "$config_file" | cut -d'=' -f2
}

# Function to get stream name from config file
get_stream_name() {
    local config_file="$1"
    grep "stream_name=" "$config_file" | cut -d'=' -f2
}

# Function to get pid file path from stream name
get_pid_file() {
    local stream_name="$1"
    local slug=$(slugify "$stream_name")
    echo "/tmp/vban_bridge_${slug}.pid"
}

# Function to check if a bridge is running
check_running() {
    local pid_file="$1"
    if [ -f "$pid_file" ]; then
        local pid=$(cat "$pid_file")
        if kill -0 "$pid" 2>/dev/null; then
            return 0  # Running
        fi
        rm "$pid_file"  # Clean up stale PID file
    fi
    return 1  # Not running
}

# Function to start a bridge
start() {
    local config_file="$1"
    local verbose="$2"
    local stream_name=$(get_stream_name "$config_file")
    local pid_file=$(get_pid_file "$stream_name")
    
    if check_running "$pid_file"; then
        echo "VBAN bridge for stream '$stream_name' is already running"
        return 1
    fi
    
    if [ "$verbose" = "true" ]; then
        ./build/simple_bridge -v -c "$config_file"
    else
        ./build/simple_bridge -c "$config_file"
    fi
    sleep 1  # Give the daemon time to start
    
    if check_running "$pid_file"; then
        echo "VBAN bridge started for stream '$stream_name'"
    else
        echo "Failed to start VBAN bridge for stream '$stream_name'"
        return 1
    fi
}

# Function to stop a bridge
stop() {
    local stream_name="$1"
    local pid_file=$(get_pid_file "$stream_name")
    
    if [ -f "$pid_file" ]; then
        local pid=$(cat "$pid_file")
        if kill -TERM "$pid" 2>/dev/null; then
            echo "Stopped VBAN bridge for stream '$stream_name'"
            rm "$pid_file"
        else
            echo "Failed to stop VBAN bridge for stream '$stream_name'"
            return 1
        fi
    else
        echo "No VBAN bridge running for stream '$stream_name'"
        return 1
    fi
}

# Function to show status
status() {
    local found=0
    for pid_file in /tmp/vban_bridge_*.pid; do
        if [ -f "$pid_file" ]; then
            local slug=$(echo "$pid_file" | sed 's/.*_\(.*\)\.pid/\1/')
            local pid=$(cat "$pid_file")
            if kill -0 "$pid" 2>/dev/null; then
                # Try to find the config file and get the actual stream name
                local stream_name=""
                for config in *.ini; do
                    if [ -f "$config" ]; then
                        local config_stream=$(get_stream_name "$config")
                        if [ "$(slugify "$config_stream")" = "$slug" ]; then
                            stream_name="$config_stream"
                            break
                        fi
                    fi
                done
                [ -z "$stream_name" ] && stream_name="$slug"
                echo "VBAN bridge running for stream '$stream_name' (PID: $pid)"
                found=1
            else
                rm "$pid_file"  # Clean up stale PID file
            fi
        fi
    done
    
    if [ $found -eq 0 ]; then
        echo "No VBAN bridges running"
    fi
}

# Main script
case "$1" in
    start)
        if [ -z "$2" ]; then
            echo "Usage: $0 start [-v] <config_file>"
            exit 1
        fi
        
        if [ "$2" = "-v" ]; then
            if [ -z "$3" ]; then
                echo "Usage: $0 start [-v] <config_file>"
                exit 1
            fi
            start "$3" "true"
        else
            start "$2" "false"
        fi
        ;;
    stop)
        if [ -z "$2" ]; then
            echo "Usage: $0 stop <stream_name>"
            exit 1
        fi
        stop "$2"
        ;;
    restart)
        if [ -z "$2" ] || [ -z "$3" ]; then
            echo "Usage: $0 restart <stream_name> <config_file>"
            exit 1
        fi
        stop "$2"
        sleep 1
        start "$3"
        ;;
    status)
        status
        ;;
    *)
        echo "Usage: $0 {start|stop|restart|status}"
        echo "  start [-v] <config_file>                Start a new VBAN bridge"
        echo "  stop <stream_name>                 Stop a VBAN bridge"
        echo "  restart <stream_name> <config_file> Restart a VBAN bridge"
        echo "  status                             Show status of all bridges"
        exit 1
        ;;
esac

exit 0 
exit 0 
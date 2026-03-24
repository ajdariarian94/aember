cat << 'EOF' > /var/lib/aember/containers/alpine/echo-alpine.sh
#!/bin/sh
while true; do
    echo "Hello from Alpine!"
    sleep 1
done
EOF

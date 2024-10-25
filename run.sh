#!/bin/bash

gcc server.c -o server


if [ $? -eq 0 ]; then
    ./server
else
    echo "error"
fi
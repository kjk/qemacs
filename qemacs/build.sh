#!/bin/bash

mkdir -p bin

GMCS=gmcs

$GMCS -target:exe -out:bin/qe.exe *.cs

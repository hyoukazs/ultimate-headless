#!/usr/bin/env python3
"""Mostra o conteúdo de resources/login-context.bin campo a campo.

Mesmo layout validado em src/headless/login.cpp (opção --official-context):
  "UHCTX1" | client_id u16 | versão u16 (860)
  3 campos: tamanho u16 + bytes  (preâmbulo, usuário do SO, CPU)
  assinaturas DAT, SPR, PIC: u32 cada
Inteiros little-endian. Só lê o arquivo; não acessa rede.
"""
import struct
import sys

path = sys.argv[1] if len(sys.argv) > 1 else "resources/login-context.bin"
data = open(path, "rb").read()
if data[:6] != b"UHCTX1":
    sys.exit("assinatura UHCTX1 ausente")
client_id, version = struct.unpack_from("<HH", data, 6)
print(f"arquivo: {path} ({len(data)} bytes)")
print(f"client_id: {client_id}  versão do protocolo: {version}")
at = 10
for label in ("preâmbulo do protocolo", "usuário do SO", "CPU"):
    (size,) = struct.unpack_from("<H", data, at)
    at += 2
    field = data[at:at + size]
    at += size
    shown = field.decode("ascii", "replace") if size else "(vazio)"
    print(f"{label}: {size} bytes: {shown}")
dat, spr, pic = struct.unpack_from("<III", data, at)
at += 12
print(f"assinaturas: DAT=0x{dat:08X} SPR=0x{spr:08X} PIC=0x{pic:08X}")
if at != len(data):
    sys.exit(f"layout inesperado: {len(data) - at} bytes sobrando")
print("Nenhum outro dado existe no arquivo.")

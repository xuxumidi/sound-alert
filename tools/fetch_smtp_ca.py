"""TLS handshake only: export the already-verified SMTP trust root. No login/mail."""
import argparse
import hashlib
from pathlib import Path
import socket
import ssl

def main():
    p=argparse.ArgumentParser()
    p.add_argument('--host',default='smtp.qq.com')
    p.add_argument('--port',type=int,default=465)
    p.add_argument('--output',type=Path,default=Path(__file__).resolve().parents[1]/'include/smtp_ca.h')
    args=p.parse_args()
    context=ssl.create_default_context()
    # This context verifies the hostname and a chain rooted in the computer's trust store.
    with socket.create_connection((args.host,args.port),timeout=20) as raw:
        with context.wrap_socket(raw,server_hostname=args.host) as peer:
            getter=getattr(peer,'get_verified_chain',None)
            if getter:
                chain=getter()
                pem=ssl.DER_cert_to_PEM_cert(chain[-1])
            else:
                # CPython 3.12 exposes the same verified chain through its SSL object.
                getter=getattr(peer._sslobj,'get_verified_chain',None)
                if not getter:
                    raise RuntimeError('Use CPython 3.12+ supporting verified-chain export, or configure a trusted CA PEM manually')
                chain=getter()
                pem=chain[-1].public_bytes()
                if isinstance(pem,bytes): pem=pem.decode('ascii')
            if not chain or 'BEGIN CERTIFICATE' not in pem:
                raise RuntimeError('No verified certificate root returned')
            subject=peer.getpeercert().get('subject')
    der=ssl.PEM_cert_to_DER_cert(pem)
    output='#pragma once\n// Verified TLS chain from '+args.host+'; public certificate, no credentials.\nnamespace mailtrust {\n'
    output+='constexpr char host[] = "'+args.host+'";\n'
    output+='constexpr char root[] = R"CA('+pem+')CA";\n}\n'
    args.output.write_text(output,encoding='utf-8')
    print('Verified host:',args.host,'leaf subject:',subject)
    print('Trust anchor SHA256:',hashlib.sha256(der).hexdigest())
    print('Saved public CA:',args.output)

if __name__=='__main__':
    main()

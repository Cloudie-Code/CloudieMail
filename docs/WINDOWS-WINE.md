# Compilar Windows pelo Wine

A build usa `cl.exe` e o linker da Microsoft. O ambiente de referência tem MSVC 19.44, Windows SDK 10.0.22621.0 e wrappers `msvc-wine` em `/opt/msvc`.

```bash
bash scripts/bootstrap-windows-wine.sh
bash scripts/build-windows-wine.sh
```

`CLOUDIEMAIL_MSVC_TOOLCHAIN` permite indicar outro arquivo de toolchain; o padrão é `/opt/msvc/toolchain.cmake`. As dependências ficam em `.deps/windows/`; os executáveis ficam em `build/windows-release/`.

| Componente | Configuração |
| --- | --- |
| Firebird 5.0.4 | Runtime embedded oficial x64, plugins, ICU, CRT e import library |
| libsodium 1.0.22 | Pacote MSVC oficial, x64 Release v143 static |
| OpenSSL 3.5.9 | Compilado do fonte com MSVC; estático, sem assembly |
| curl 8.23.0 | Biblioteca estática, backend TLS Schannel |
| nlohmann-json 3.12.0 | Headers portáveis |
| Lego 5.6.0 | Binário oficial Windows amd64 para ACME DNS-01 |

O script usa `/MT`. OpenSSL recebe `-Z7` para manter as informações de depuração nos objetos e evitar disputa de PDB no Wine. Firebird mantém suas DLLs de runtime.

Para executar manualmente:

```bash
source scripts/windows-wine-env.sh
wine build/windows-release/cloudiemail.exe --help
wine build/windows-release/cloudiemail-gui.exe config/example.json
```

## Limitações do ambiente

Os testes pelo Wine não substituem testes no Windows. Foram observadas estas diferenças:

- Firebird embedded falha ao inicializar alguns mapeamentos de memória. A suíte completa de banco/protocolos precisa rodar no Windows nativo.
- O teste estrito de ACL não passou na emulação de permissões. Não há aprovação de permissões NTFS baseada no Wine.
- O teste de recusa de certificado autoassinado sem CA explícita falhou com Schannel sob Wine. A validação dessa condição no Windows nativo permanece pendente; a verificação TLS não foi desativada no código.
- Os testes do adaptador HTTP.sys validam requisições e cabeçalhos, mas não abrem um listener TLS HTTP.sys nativo.

`bash scripts/build-windows-wine.sh --full-tests` tenta a suíte completa. A opção normal compila e roda os testes criptográficos compatíveis. O comando `serve CONFIG --run-for 30` permite encerrar o servidor após 30 segundos em testes que não podem usar sinais Unix.

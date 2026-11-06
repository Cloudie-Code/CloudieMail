# Preparar uma release

1. Atualize a versão em `CMakeLists.txt`, as notas e os limites conhecidos.
2. Compile Linux e Windows e execute os testes relevantes. Registre falhas e testes que não puderam ser executados em `STATUS.md`.
3. Gere as capturas com contas de demonstração e confira as imagens antes de incluí-las.
4. Execute `python3 scripts/package.py --replace-pristine` e `python3 tests/package_smoke.py`.
5. Execute `python3 scripts/release.py --version 0.2.0`. O script verifica os manifests, recusa dados de instalação e gera os dois arquivos e seus checksums em `Output/releases/v0.2.0/`.
6. Revise os arquivos que entrarão no Git, faça o commit e publique a tag. Use pré-release enquanto houver limitações que impeçam uma versão estável.
7. Anexe os pacotes e `SHA256SUMS` à release. Não publique bancos, tokens, certificados pessoais ou chaves privadas.

O repositório contém fontes, documentação e imagens. Dependências baixadas, builds e pacotes ficam fora do Git. Os arquivos de release devem ser produzidos a partir do mesmo código da tag.

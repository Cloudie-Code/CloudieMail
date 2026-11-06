# CloudieMail

- Criar commits, remotos, tags e releases somente quando solicitado.
- Não incluir trailers Co-authored-by sem pedido explícito para aquele commit.
- C++20; GTK4 no Linux; Win32/MSVC no Windows; Firebird 5 para persistência.
- Não implementar primitivas criptográficas próprias. Usar libsodium e OpenSSL.
- Domínio e timestamp identificam a chave; nunca são fonte de entropia.
- Não declarar segurança absoluta nem prontidão para produção sem validação.
- Testar alterações em autenticação, criptografia, isolamento e protocolos.

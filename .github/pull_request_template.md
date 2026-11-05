## Ringkasan

<!-- Apa yang berubah dan kenapa. Satu paragraf cukup. -->

## Jenis perubahan

- [ ] Fitur baru
- [ ] Perbaikan bug
- [ ] Operasi / deployment
- [ ] Dokumentasi
- [ ] Refactor / housekeeping

## Verifikasi

<!-- Tempel perintah dan hasil singkat, bukan sekadar "sudah dites". -->

```bash
./build.sh
./build/xcb --selftest
(cd build && ctest --output-on-failure)
shellcheck -S warning $(git ls-files '*.sh')
```

## Risiko & rollback

<!-- Apa yang bisa rusak, dan bagaimana mengembalikannya. -->

autocmd FileType h,c set expandtab ts=2 sw=2 tw=79 foldmethod=syntax foldenable

set colorcolumn=80
highlight ColorColumn ctermbg=darkgray
set number
let g:syntastic_c_include_dirs = ['cfg']
let g:syntastic_c_compiler_options = "-std=c99 -Wall -Wextra -Wpedantic"
let g:syntastic_loc_list_height=5
let g:syntastic_mode_map = { 'mode': 'passive' }
let g:ctrlp_custom_ignore = {
  \ 'dir':  '\v[\/]\.(bin)$',
  \ 'file': '\v\.(o)$',
  \ }
let g:ycm_confirm_extra_conf = 0

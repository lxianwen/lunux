make -j $(nproc)\
    && sudo make -j $(nproc) INSTALL_MOD_STRIP=1 modules_install && sudo make -j $(nproc) install

# make INSTALL_MOD_STRIP=1 modules_install
# # 编译所有（包括内核和模块）
# make -j 64
# # 安装模块
# sudo make modules_install
# # 安装内核
# sudo make install
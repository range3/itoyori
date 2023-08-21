FROM range3/dev-cpp:1.1.1

USER root
RUN \
  # sshd
  export DEBIAN_FRONTEND=noninteractive \
  && apt-get -y install --no-install-recommends \
    openssh-server \
  # sshd_config
  && printf '%s\n' \
    'PasswordAuthentication yes' \
    'PermitEmptyPasswords yes' \
    'UsePAM no' \
    > /etc/ssh/sshd_config.d/auth.conf \
  # ssh_config
  && printf '%s\n' \
    'Host *' \
    '    StrictHostKeyChecking no' \
    > /etc/ssh/ssh_config.d/ignore-host-key.conf \
  # delete passwd
  && passwd -d vscode \
  # Clean up
  && apt-get autoremove -y \
  && apt-get clean -y \
  && rm -rf /var/lib/apt/lists/*

RUN \
  # additinal packages
  export DEBIAN_FRONTEND=noninteractive \
  && apt-get -y install --no-install-recommends \
    flex \
  # Clean up
  && apt-get autoremove -y \
  && apt-get clean -y \
  && rm -rf /var/lib/apt/lists/*
